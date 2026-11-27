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


# With zero hold, cross the threshold exactly at two consecutive chunk boundaries.
alignment = 1680
gate = GateHold(.1, 0, alignment)
quiet = np.zeros(5 * hop, np.float32)
voiced = np.full_like(quiet, .4)
attack = gate.window(voiced)
release = gate.window(quiet)
step = np.arange(1, 97) / 96
np.testing.assert_allclose(attack[:alignment], 0)
np.testing.assert_allclose(attack[alignment:alignment+96], step, atol=1e-6)
np.testing.assert_allclose(release[:alignment], 1, atol=1e-6)
np.testing.assert_allclose(release[alignment:alignment+96], 1 - step, atol=1e-6)
assert not release[alignment+96:].any() and not gate.window(quiet).any()
burst = quiet.copy()
burst[:hop] = .2  # Above threshold in one hop, below threshold across the chunk.
assert not gate.window(burst).any()
print("PASS zero-hold chunk RMS and 96-sample opening and closing ramps")
