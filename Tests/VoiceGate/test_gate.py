"""Check the hold duration and decoder alignment independently of model weights."""
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Resources" / "RVC"))
from gate import GateHold

hop = GateHold.hop
signal = np.zeros(100 * hop, np.float32)
signal[5*hop:7*hop] = .4
signal[12*hop:14*hop] = .4  # Reopen during the first hold.
signal[50*hop:51*hop] = .4  # Later utterance after a fully closed gate.
hold = 75
reference = None
for chunk in (5, 10, 25, 50):
    overlap = 1920
    gate = GateHold(.1, hold, overlap)
    emitted = []
    for block in signal.reshape(-1, chunk * hop):
        window = gate.window(block)
        history = np.concatenate(emitted[-10:]) if emitted else np.array([])
        np.testing.assert_allclose(window[:overlap], np.pad(history, (overlap, 0))[-overlap:])
        emitted.append(window[overlap:])
    out = np.concatenate(emitted)
    if reference is not None:
        np.testing.assert_allclose(out, reference)
    reference = out
    assert not out[:5*hop].any()
    assert np.min(out[5*hop+96:14*hop+hold*48]) > .999
    assert not out[14*hop+hold*48+96:50*hop].any()
    assert not out[51*hop+hold*48+96:].any()
print("PASS sample-timed gate hold, retrigger, silence, decoder alignment and chunk boundaries")
