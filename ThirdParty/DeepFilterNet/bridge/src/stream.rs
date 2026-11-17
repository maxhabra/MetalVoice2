//! Streaming DSP around an externally accelerated neural predictor.
//! Uses upstream's FFT/window, ERB bands, normalization, mask interpolation,
//! synthesis, thresholds and spectrum alignment. The oracle test verifies the
//! assembly against the unmodified upstream DfTract process implementation.
use df::{Complex32, DFState};
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};

pub type Predict = unsafe extern "C" fn(
    *mut c_void, *const f32, *const f32, *mut f32, *mut f32, *mut f32,
) -> i32;

pub struct Stream {
    dsp: DFState,
    pristine: DFState,
    spectra: [[Complex32; 481]; 5],
    enhanced: [Complex32; 481],
    erb: [f32; 32],
    complex: [Complex32; 96],
    features: [f32; 192],
    gains: [f32; 32],
    coefficients: [f32; 960],
    skip_counter: usize,
    predict: Predict,
    context: *mut c_void,
    failed: bool,
}

impl Stream {
    fn new(predict: Predict, context: *mut c_void) -> Self {
        let mut dsp = DFState::new(48000, 960, 480, 32, 2);
        dsp.init_norm_states(96);
        Self { pristine: dsp.clone(), dsp, spectra: [[Complex32::default(); 481]; 5],
            enhanced: [Complex32::default(); 481], erb: [0.; 32], complex: [Complex32::default(); 96],
            features: [0.; 192], gains: [0.; 32], coefficients: [0.; 960], skip_counter: 0,
            predict, context, failed: false }
    }

    fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<(), ()> {
        let energy = input.iter().fold(0f32, |acc, x| acc + x.powi(2)) / 480.;
        if energy < 1e-7 { self.skip_counter += 1; } else { self.skip_counter = 0; }
        if self.skip_counter > 5 { output.fill(0.); return Ok(()); }
        self.spectra.rotate_left(1);
        self.dsp.analysis(input, &mut self.spectra[4]);
        // Upstream calc_norm_alpha(48000, 480, 1) rounds to 0.990.
        self.dsp.feat_erb(&self.spectra[4], 0.990, &mut self.erb);
        self.dsp.feat_cplx(&self.spectra[4][..96], 0.990, &mut self.complex);
        for (i, c) in self.complex.iter().enumerate() {
            self.features[i] = c.re;
            self.features[96+i] = c.im;
        }
        let mut lsnr = 0.;
        if unsafe { (self.predict)(self.context, self.erb.as_ptr(), self.features.as_ptr(),
            self.gains.as_mut_ptr(), self.coefficients.as_mut_ptr(), &mut lsnr) } != 0
            || !lsnr.is_finite() || !self.gains.iter().chain(self.coefficients.iter()).all(|v| v.is_finite()) {
            return Err(());
        }
        // A five-frame chronological ring: the center is the signal two hops
        // behind current capture. Older masked spectra are never reused by DF.
        self.enhanced.copy_from_slice(&self.spectra[2]);
        if lsnr < -10. {
            self.enhanced.fill(Complex32::default());
            self.skip_counter = 0;
        } else if lsnr <= 30. {
            self.dsp.apply_mask(&mut self.enhanced, &self.gains);
            self.skip_counter = 0;
        } else { self.skip_counter += 1; }
        if (-10. ..=20.).contains(&lsnr) {
            self.enhanced[..96].fill(Complex32::default());
            for (lag, spectrum) in self.spectra.iter().enumerate() {
                for (bin, (value, out)) in spectrum.iter().zip(self.enhanced.iter_mut()).take(96).enumerate() {
                    let i = bin * 10 + lag * 2;
                    *out += *value * Complex32::new(self.coefficients[i], self.coefficients[i+1]);
                }
            }
        }
        self.dsp.synthesis(&mut self.enhanced, output);
        if !output.iter().all(|s| s.is_finite()) { return Err(()); }
        Ok(())
    }
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_stream_create(predict: Option<Predict>, context: *mut c_void) -> *mut Stream {
    let Some(predict) = predict else { return std::ptr::null_mut(); };
    match catch_unwind(AssertUnwindSafe(|| Stream::new(predict, context))) {
        Ok(stream) => Box::into_raw(Box::new(stream)), Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_stream_destroy(stream: *mut Stream) {
    if !stream.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(stream)))); }
}

// The owner resets its neural state separately, on the same worker thread.
#[no_mangle]
pub unsafe extern "C" fn mv_df_stream_reset(stream: *mut Stream) -> i32 {
    let Some(stream) = stream.as_mut() else { return -1; };
    stream.failed = true;
    if catch_unwind(AssertUnwindSafe(|| {
        stream.dsp = stream.pristine.clone();
        stream.spectra.fill([Complex32::default(); 481]);
        stream.skip_counter = 0;
    })).is_err() { return -1; }
    stream.failed = false;
    0
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_stream_process(stream: *mut Stream, input: *const f32, output: *mut f32) -> i32 {
    let Some(stream) = stream.as_mut() else { return -1; };
    if stream.failed || input.is_null() || output.is_null() || (input as usize).abs_diff(output as usize) < 480*4 { return -1; }
    let input = std::slice::from_raw_parts(input, 480);
    let output = std::slice::from_raw_parts_mut(output, 480);
    if !input.iter().all(|s| s.is_finite()) { output.fill(0.); return -1; }
    if matches!(catch_unwind(AssertUnwindSafe(|| stream.process(input, output))), Ok(Ok(()))) { return 0; }
    output.fill(0.);
    stream.failed = true;
    -1
}

#[cfg(test)]
mod tests {
    use super::*;
    use ndarray::{ArrayView2, ArrayViewMut2};
    struct Oracle { model: df::tract::DfTract, analysis: DFState, input: [f32; 480] }
    unsafe extern "C" fn predict(context: *mut c_void, erb: *const f32, features: *const f32,
        gains: *mut f32, coefficients: *mut f32, lsnr: *mut f32) -> i32 {
        catch_unwind(AssertUnwindSafe(|| {
        let oracle = &mut *context.cast::<Oracle>();
        let mut spec = [Complex32::default(); 481];
        oracle.analysis.analysis(&oracle.input, &mut spec);
        let mut expected_erb = [0.; 32]; let mut expected_spec = [Complex32::default(); 96];
        oracle.analysis.feat_erb(&spec, 0.990, &mut expected_erb);
        oracle.analysis.feat_cplx(&spec[..96], 0.990, &mut expected_spec);
        assert_eq!(std::slice::from_raw_parts(erb, 32), expected_erb);
        let actual_spec = std::slice::from_raw_parts(features, 192);
        for i in 0..96 { assert_eq!(actual_spec[i], expected_spec[i].re); assert_eq!(actual_spec[96+i], expected_spec[i].im); }
        oracle.model.get_mut_spec_enh().as_slice_mut().unwrap().copy_from_slice(&spec);
        let (snr, mask, coefs) = oracle.model.process_raw().unwrap();
        *lsnr = snr;
        let gain_output = std::slice::from_raw_parts_mut(gains, 32); gain_output.fill(0.);
        let coef_output = std::slice::from_raw_parts_mut(coefficients, 960); coef_output.fill(0.);
        if let Some(m) = mask { gain_output.copy_from_slice(m.as_slice::<f32>().unwrap()); }
        if let Some(c) = coefs { coef_output.copy_from_slice(c.as_slice::<f32>().unwrap()); }
        0
        })).unwrap_or(-1)
    }
    #[test]
    fn accelerated_dsp_matches_complete_upstream_pipeline() {
        for repetition in 0..2 {
            let mut reference = super::super::reference().unwrap();
            let mut analysis = DFState::default(); analysis.init_norm_states(96);
            let mut oracle = Oracle { model: super::super::reference().unwrap(), analysis, input: [0.;480] };
            let mut stream = Stream::new(predict, (&mut oracle as *mut Oracle).cast());
            let mut seed = 42u32;
            for frame in 0..300 {
                for i in 0..480 {
                    seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                    oracle.input[i] = if frame > 200 && frame < 230 { 0. } else {
                        0.1 * (((frame*480+i) as f32) * 0.023).sin() + 0.04 * ((seed>>8) as f32 / 8388608. - 1.)
                    };
                }
                let mut expected = [0.;480]; let mut actual = [0.;480];
                reference.process(ArrayView2::from_shape((1,480), &oracle.input).unwrap(), ArrayViewMut2::from_shape((1,480), &mut expected).unwrap()).unwrap();
                let input = oracle.input;
                stream.process(&input, &mut actual).unwrap();
                assert_eq!(actual, expected, "frame {frame}, repeat {repetition}");
            }
        }
    }
}
