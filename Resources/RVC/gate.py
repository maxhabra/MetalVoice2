"""Speech gate timing for the 48 kHz RVC worker."""
import numpy as np


class GateHold:
    hop = 480

    def __init__(self, threshold, milliseconds, alignment_samples):
        self.threshold_squared = threshold ** 2
        self.hold = round(milliseconds * 48)
        self.remaining = 0
        # A 2 ms linear transition avoids clicks, including at chunk boundaries.
        self.ramp = np.full(96, 1 / 96, np.float32)
        self.ramp_history = np.zeros(len(self.ramp) - 1, np.float32)
        self.history = np.zeros(alignment_samples, np.float32)

    def window(self, block):
        """Keep the mask on the same input timeline as the decoder's overlap/search window."""
        # Zero hold uses the original whole-chunk RMS decision. Both modes ramp
        # the resulting mask on the same sample timeline as the decoded audio.
        frames = block.reshape(-1, self.hop if self.hold else block.size)
        above = np.mean(frames * frames, axis=1) >= self.threshold_squared
        mask = np.zeros_like(frames)
        for i, voiced in enumerate(above):
            if voiced:
                mask[i].fill(1)
                self.remaining = self.hold
            elif self.remaining:
                count = min(self.remaining, self.hop)
                mask[i, :count] = 1
                self.remaining -= count
        raw = np.concatenate((self.ramp_history, mask.ravel()))
        self.ramp_history[:] = raw[-len(self.ramp_history):]
        envelope = np.convolve(raw, self.ramp, mode="valid")
        window = np.concatenate((self.history, envelope))
        self.history[:] = window[-self.history.size:]
        return window
