#!/usr/bin/env python3
"""Independent ONNX oracle versus native Core ML + Accelerate, no microphone."""
import argparse
import ctypes as C
import json
from pathlib import Path
import time
import numpy as np
import onnxruntime as ort

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--units', type=int, default=0)
    parser.add_argument('--explore', action='store_true', help='Report candidate errors without accepting parity')
    parser.add_argument('--model', type=Path, default=ROOT / 'build/DPDFNet2.mlmodelc')
    args = parser.parse_args()
    options = ort.SessionOptions()
    options.intra_op_num_threads = options.inter_op_num_threads = 1
    ref = ort.InferenceSession(str(ROOT / 'ThirdParty/DPDFNet/upstream/models/dpdfnet2_48khz_hr.onnx'), options, providers=['CPUExecutionProvider'])
    meta = ref.get_modelmeta().custom_metadata_map
    initial = np.zeros(int(meta['state_size']), np.float32)
    n = int(meta['erb_norm_state_size'])
    initial[:n] = np.fromstring(meta['erb_norm_init'], sep=',', dtype=np.float32)
    initial[n:n+int(meta['spec_norm_state_size'])] = np.fromstring(meta['spec_norm_init'], sep=',', dtype=np.float32)
    np.testing.assert_array_equal(initial, np.fromfile(ROOT / 'Resources/DPDFNet2.state.f32', dtype=np.float32))
    lib = C.CDLL(str(ROOT / 'build/libDPDFTest.dylib'))
    pointer = np.ctypeslib.ndpointer(dtype=np.float32, flags='C_CONTIGUOUS')
    lib.mv_dpdf_create.argtypes = [C.c_char_p, C.c_char_p, C.c_int, C.c_char_p, C.c_size_t]
    lib.mv_dpdf_create.restype = C.c_void_p
    lib.mv_dpdf_predict.argtypes = [C.c_void_p, pointer, pointer]
    lib.mv_dpdf_process.argtypes = [C.c_void_p, pointer, pointer]
    lib.mv_dpdf_reset.argtypes = [C.c_void_p]
    lib.mv_dpdf_destroy.argtypes = [C.c_void_p]
    lib.mv_dpdf_devices.argtypes = [C.c_void_p]
    error = C.create_string_buffer(1024)
    start = time.perf_counter()
    native = lib.mv_dpdf_create(str(args.model).encode(), str(ROOT / 'Resources/DPDFNet2.state.f32').encode(), args.units, error, len(error))
    assert native, error.value
    print('initialization_ms', (time.perf_counter()-start)*1000, 'preferred_devices', lib.mv_dpdf_devices(native), flush=True)
    try:
        rng = np.random.default_rng(2026)
        state = initial.copy()
        actual = np.zeros((1,1,481,2), np.float32)
        max_error = 0.
        for frame in range(150):
            spec = rng.normal(0, 0.1 if frame < 100 else 0, actual.shape).astype(np.float32)
            expected, state = ref.run(None, {'spec': spec, 'state_in': state})
            assert lib.mv_dpdf_predict(native, spec, actual) == 0
            max_error = max(max_error, float(np.max(np.abs(actual-expected))))
        print('spectral_max_abs_error', max_error, flush=True)
        assert args.explore or max_error < 1e-4
        assert lib.mv_dpdf_reset(native) == 0
        speech = np.fromfile(ROOT / 'build/deepfilter-speech.f32', dtype=np.float32)
        speech = speech[:len(speech)//480*480]
        speech += rng.normal(0, .015, speech.shape).astype(np.float32)
        audio = np.concatenate([np.zeros(48000, np.float32), speech,
                                np.zeros(48000*4, np.float32), speech, np.zeros(48000, np.float32)])
        audio = np.clip(audio, -1, 1)
        window = np.sin(np.pi/2*np.sin(np.pi*(np.arange(960)+.5)/960)**2).astype(np.float32)
        overlap = np.zeros(960, np.float32)
        expected = np.zeros_like(audio)
        actual = np.zeros_like(audio)
        state = initial.copy()
        times = []
        for pos in range(0, len(audio), 480):
            if pos:
                spectrum = np.fft.rfft(audio[pos-480:pos+480]*window)
                spec = np.stack([spectrum.real, spectrum.imag], axis=-1).astype(np.float32)[None,None]
                out, state = ref.run(None, {'spec':spec, 'state_in':state})
                frame = (np.fft.irfft(out[0,0,:,0]+1j*out[0,0,:,1], n=960)*window).astype(np.float32)
                overlap += frame
                expected[pos:pos+480] = overlap[:480]
                overlap[:480] = overlap[480:]; overlap[480:] = 0
            start = time.perf_counter()
            assert lib.mv_dpdf_process(native, audio[pos:pos+480], actual[pos:pos+480]) == 0
            times.append((time.perf_counter()-start)*1000)
        delta = actual-expected
        relative = float(np.linalg.norm(delta)/np.linalg.norm(expected))
        print(json.dumps(dict(audio_max_abs_error=float(np.max(np.abs(delta))), relative_rms_error=relative,
                              compute_ms=np.percentile(times[20:], [50,99,100]).tolist())), flush=True)
        assert args.explore or (relative < 1e-4 and np.max(np.abs(delta)) < 1e-4)
        audio.tofile(ROOT / 'build/dpdfnet-input.f32')
        expected.tofile(ROOT / 'build/dpdfnet-reference.f32')
        # Broad-band correlation validates signal alignment separately from compute.
        length = 1 << (2*len(audio)-1).bit_length()
        cross = np.fft.irfft(np.fft.rfft(expected, length)*np.conj(np.fft.rfft(audio, length)), length)
        delay = int(np.argmax(cross[:4800]))
        print('signal_delay_samples', delay, 'ms', delay/48, flush=True)
        assert delay == 2400
    finally:
        lib.mv_dpdf_destroy(native)


if __name__ == '__main__':
    main()
