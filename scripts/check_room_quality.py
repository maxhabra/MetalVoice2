#!/usr/bin/env python3
"""Compare native room DSP on supplied 48 kHz mono Float32 speech + simulated rooms.
Development-only NumPy probe: no recording, no networking, no perceptual claims.
Run from the repository root using the model-export Python environment.
"""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import time
import numpy as np

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('speech', type=Path)
args = parser.parse_args()
speech = np.fromfile(args.speech, dtype=np.float32)
if len(speech) < 96000 or not np.all(np.isfinite(speech)) or np.linalg.norm(speech) == 0:
    raise SystemExit('Supply at least two seconds of finite, non-silent 48 kHz float32 speech.')
folder = Path('build/wpe-reference')
folder.mkdir(parents=True, exist_ok=True)
(folder/'bridge.cpp').write_text('''#include "RoomDereverb.hpp"
extern "C" void *create() { return new mv::RoomDereverb(); }
extern "C" bool process(void *s, float *p) { return static_cast<mv::RoomDereverb *>(s)->process(p); }
extern "C" void destroy(void *s) { delete static_cast<mv::RoomDereverb *>(s); }
''')
subprocess.run(['clang++', '-O2', '-std=c++20', '-shared', '-fPIC', '-I', 'Sources/AudioCore/include',
                'Sources/AudioCore/RoomDereverb.cpp', str(folder/'bridge.cpp'), '-framework', 'Accelerate',
                '-o', str(folder/'libRoom.dylib')], check=True)
lib = ctypes.CDLL(str(folder/'libRoom.dylib'))
lib.create.restype = ctypes.c_void_p
lib.process.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float)]
lib.process.restype = ctypes.c_bool
lib.destroy.argtypes = [ctypes.c_void_p]


def run(audio):
    padded = np.pad(audio, (0, (-len(audio)) % 480 + 480)).astype(np.float32)
    output, times = [], []
    state = lib.create()
    try:
        for block in padded.reshape(-1, 480):
            block = block.copy()
            before = time.perf_counter()
            if not lib.process(state, block.ctypes.data_as(ctypes.POINTER(ctypes.c_float))):
                raise RuntimeError('Native room processing failed')
            times.append((time.perf_counter()-before)*1000)
            output.append(block)
    finally:
        lib.destroy(state)
    return np.concatenate(output)[480:480+len(audio)], np.percentile(times, [50, 99])


def sisdr(observed, target):
    # Align before scoring and exclude one second of adaptation. SI-SDR is not MOS.
    observed, target = observed[48000:].astype(np.float64), target[48000:].astype(np.float64)
    scaled = target * (np.dot(observed, target) / np.dot(target, target))
    return float(10*np.log10(np.sum(scaled**2)/max(np.sum((observed-scaled)**2), 1e-30)))


results = []
for name, rt60 in [('dry', 0), ('small', .3), ('medium', .6), ('large', .9)]:
    rng = np.random.default_rng(823)
    rir = np.zeros(48000)
    rir[0] = 1
    if rt60:
        for ms, amplitude in [(17, .15), (37, .2), (51, .2), (73, .17), (98, .13)]:
            rir[ms*48] = amplitude
        t = np.arange(45600)/48000
        rir[2400:] += rng.normal(size=len(t))*np.exp(-np.log(1000)*t/rt60)*.013
    size = 1 << (len(speech)+len(rir)-2).bit_length()
    observed = np.fft.irfft(np.fft.rfft(speech,size)*np.fft.rfft(rir,size),size)[:len(speech)].astype(np.float32)
    output, timing = run(observed)
    metrics = dict(room=name, rt60=rt60, inputSiSDR=sisdr(observed,speech), outputSiSDR=sisdr(output,speech),
                   levelChangeDB=float(20*np.log10(np.linalg.norm(output)/np.linalg.norm(observed))),
                   p50Ms=float(timing[0]), p99Ms=float(timing[1]))
    results.append(metrics)
    print(json.dumps(metrics), flush=True)
    np.savez(folder/(name+'.npz'), dry=speech, input=observed, output=output)
(folder/'quality.json').write_text(json.dumps(results, indent=2)+'\n')
