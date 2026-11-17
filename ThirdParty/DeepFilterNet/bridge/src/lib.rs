use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use std::ffi::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
mod stream;

const MODEL: &[u8] = include_bytes!("../../upstream/models/DeepFilterNet3_onnx.tar.gz");
const FRAME: usize = 480;

// DfTract is intentionally not Send. The native owner must create, use, reset,
// and destroy this opaque state on the same dedicated worker thread.
pub struct State {
    processor: DfTract,
    // Tract's model plans are shared by clones; this stores pristine recurrent,
    // normalization, spectrum and overlap state for a complete stream reset.
    pristine: DfTract,
    failed: bool,
    last_snr: f32,
}

fn reference() -> Result<DfTract, String> {
    let params = DfParams::from_bytes(MODEL).map_err(|e| e.to_string())?;
    let model = DfTract::new(params, &RuntimeParams::default_with_ch(1))
        .map_err(|e| e.to_string())?;
    if model.sr != 48000 || model.hop_size != FRAME || delay(&model) != 1440 {
        return Err("Unexpected DeepFilterNet3 sample rate, frame size, or delay".into());
    }
    Ok(model)
}

fn delay(model: &DfTract) -> usize {
    model.fft_size - model.hop_size + model.lookahead * model.hop_size
}

impl State {
    fn new() -> Result<Self, String> {
        let pristine = reference()?;
        let mut processor = pristine.clone();
        // Warm the complete inference path off the hardware callbacks, then
        // restore every state. No microphone audio is used for warm-up.
        let input: Vec<f32> = (0..FRAME).map(|i| 0.05 * (i as f32 * 0.043).sin()).collect();
        let mut output = [0.0; FRAME];
        for _ in 0..4 {
            processor.process(
                ArrayView2::from_shape((1, FRAME), &input).unwrap(),
                ArrayViewMut2::from_shape((1, FRAME), &mut output).unwrap(),
            ).map_err(|e| e.to_string())?;
        }
        processor = pristine.clone();
        Ok(Self { processor, pristine, failed: false, last_snr: 0. })
    }
}

unsafe fn write_error(dst: *mut c_char, capacity: usize, message: &str) {
    if dst.is_null() || capacity == 0 { return; }
    let count = message.len().min(capacity - 1);
    std::ptr::copy_nonoverlapping(message.as_ptr(), dst.cast::<u8>(), count);
    *dst.add(count) = 0;
}

/// All state operations must run on the creating thread, outside audio callbacks.
/// Error destination, if supplied, must be writable for `capacity` bytes.
#[no_mangle]
pub unsafe extern "C" fn mv_df_create(error: *mut c_char, capacity: usize) -> *mut State {
    match catch_unwind(AssertUnwindSafe(State::new)) {
        Ok(Ok(state)) => Box::into_raw(Box::new(state)),
        Ok(Err(message)) => { write_error(error, capacity, &message); std::ptr::null_mut() }
        Err(_) => { write_error(error, capacity, "DeepFilterNet3 initialization failed"); std::ptr::null_mut() }
    }
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_destroy(state: *mut State) {
    if !state.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(state)))); }
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_reset(state: *mut State) -> i32 {
    let Some(state) = state.as_mut() else { return -1; };
    state.failed = true;
    if catch_unwind(AssertUnwindSafe(|| state.processor = state.pristine.clone())).is_err() { return -1; }
    state.failed = false;
    0
}

#[no_mangle]
pub unsafe extern "C" fn mv_df_delay_frames(state: *const State) -> u32 {
    state.as_ref().map(|s| delay(&s.processor) as u32).unwrap_or(0)
}

/// Exactly 480 normalized samples. Input/output must be valid, disjoint buffers.
/// Returns 0 on success, -1 on failure; never unwinds across the C boundary.
#[no_mangle]
pub unsafe extern "C" fn mv_df_process(state: *mut State, input: *const f32, output: *mut f32) -> i32 {
    let Some(state) = state.as_mut() else { return -1; };
    if input.is_null() || output.is_null() || state.failed { return -1; }
    let bytes = FRAME * std::mem::size_of::<f32>();
    if (input as usize).abs_diff(output as usize) < bytes { return -1; }
    let input = std::slice::from_raw_parts(input, FRAME);
    let output = std::slice::from_raw_parts_mut(output, FRAME);
    if !input.iter().all(|s| s.is_finite()) { output.fill(0.0); return -1; }
    let result = catch_unwind(AssertUnwindSafe(|| state.processor.process(
        ArrayView2::from_shape((1, FRAME), input).unwrap(),
        ArrayViewMut2::from_shape((1, FRAME), &mut *output).unwrap(),
    )));
    if let Ok(Ok(snr)) = result {
        if output.iter().all(|s| s.is_finite()) { state.last_snr = snr; return 0; }
    }
    output.fill(0.0);
    state.failed = true;
    -1
}

// Offline parity diagnostics; call from the same owner thread as processing.
#[no_mangle]
pub unsafe extern "C" fn mv_df_last_snr(state: *const State) -> f32 {
    state.as_ref().map(|s| s.last_snr).unwrap_or(f32::NAN)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input_frame(frame: usize, seed: &mut u32) -> [f32; FRAME] {
        let mut input = [0.0; FRAME];
        for (i, sample) in input.iter_mut().enumerate() {
            *seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let noise = (*seed >> 8) as f32 / 8388608.0 - 1.0;
            let phase = (frame * FRAME + i) as f32 / 48000.0 * std::f32::consts::TAU;
            *sample = 0.12 * (phase * 173.0).sin() + 0.05 * (phase * 519.0).sin() + 0.03 * noise;
        }
        input
    }

    #[test]
    fn ffi_matches_upstream_and_reset_restores_all_history() {
        unsafe {
            let state = mv_df_create(std::ptr::null_mut(), 0);
            assert!(!state.is_null());
            assert_eq!(mv_df_delay_frames(state), 1440);
            for _ in 0..3 {
                let mut upstream = reference().unwrap();
                assert_eq!(mv_df_reset(state), 0);
                let mut seed = 42;
                for frame in 0..150 {
                    let input = input_frame(frame, &mut seed);
                    let mut reference_output = [0.0; FRAME];
                    upstream.process(
                        ArrayView2::from_shape((1, FRAME), &input).unwrap(),
                        ArrayViewMut2::from_shape((1, FRAME), &mut reference_output).unwrap(),
                    ).unwrap();
                    let mut output = [0.0; FRAME];
                    assert_eq!(mv_df_process(state, input.as_ptr(), output.as_mut_ptr()), 0);
                    assert_eq!(output, reference_output);
                }
            }
            mv_df_destroy(state);
        }
    }

    #[test]
    fn reported_delay_matches_upstream_signal_alignment() {
        // A small attenuation limit retains almost all original spectrum while
        // still executing inference, exposing window/lookahead alignment
        // independently of time-varying suppression and deep-filter phase.
        for offset in [0, 1, 137, 479] {
            let mut model = reference().unwrap();
            model.set_atten_lim(0.1);
            let mut response = Vec::new();
            for frame in 0..7 {
                let mut input = [0.0; FRAME];
                if frame == 0 { input[offset] = 0.5; }
                let mut output = [0.0; FRAME];
                model.process(
                    ArrayView2::from_shape((1, FRAME), &input).unwrap(),
                    ArrayViewMut2::from_shape((1, FRAME), &mut output).unwrap(),
                ).unwrap();
                response.extend_from_slice(&output);
            }
            let peak = response.iter().enumerate().max_by(|a, b| a.1.abs().total_cmp(&b.1.abs())).unwrap();
            assert!(peak.1.abs() > 0.1);
            assert_eq!(peak.0 - offset, delay(&model));
        }
    }

    #[test]
    fn ffi_rejects_bad_inputs_without_corrupting_state() {
        unsafe {
            assert_eq!(mv_df_reset(std::ptr::null_mut()), -1);
            let state = mv_df_create(std::ptr::null_mut(), 0);
            assert!(!state.is_null());
            let mut samples = [0.0; FRAME];
            assert_eq!(mv_df_process(state, samples.as_ptr(), samples.as_mut_ptr()), -1);
            let mut output = [1.0; FRAME];
            samples[10] = f32::NAN;
            assert_eq!(mv_df_process(state, samples.as_ptr(), output.as_mut_ptr()), -1);
            assert!(output.iter().all(|s| *s == 0.0));
            samples.fill(0.0);
            assert_eq!(mv_df_process(state, samples.as_ptr(), output.as_mut_ptr()), 0);
            assert!(output.iter().all(|s| *s == 0.0));
            mv_df_destroy(state);
        }
    }
}
