"""WORLD Harvest on RVC's 16 kHz input and 10 ms pitch grid."""
import ctypes as C
from pathlib import Path

import numpy as np


class HarvestOptions(C.Structure):
    _fields_ = [("f0_floor", C.c_double), ("f0_ceil", C.c_double), ("frame_period", C.c_double)]


class HarvestPitch:
    def __init__(self):
        library = Path(__file__).resolve().parents[2] / "Frameworks" / "libWorldHarvest.dylib"
        self.world = C.CDLL(str(library))
        signal = np.ctypeslib.ndpointer(dtype=np.float64, ndim=1, flags="C_CONTIGUOUS")
        self.world.Harvest.argtypes = [signal, C.c_int, C.c_int, C.POINTER(HarvestOptions), signal, signal]
        self.world.Harvest.restype = None
        self.world.GetSamplesForHarvest.argtypes = [C.c_int, C.c_int, C.c_double]
        self.world.GetSamplesForHarvest.restype = C.c_int
        self.options = HarvestOptions(50, 1100, 10)

    def estimate(self, audio):
        waveform = np.ascontiguousarray(audio, dtype=np.float64)
        frames = self.world.GetSamplesForHarvest(16000, waveform.size, self.options.frame_period)
        times = np.empty(frames, np.float64)
        pitch = np.empty_like(times)
        self.world.Harvest(waveform, waveform.size, 16000, C.byref(self.options), times, pitch)
        return pitch.astype(np.float32)
