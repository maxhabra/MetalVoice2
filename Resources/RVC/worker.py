"""Private 48 kHz mono float32 pipe used by the macOS preview."""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import sys

# Keep library diagnostics off the PCM channel, including native writes to stdout.
pcm = os.fdopen(os.dup(1), "wb")
os.dup2(2, 1)
sys.path.insert(0, str(Path(__file__).resolve().parent))


def lowpass(hz, rate, taps):
    x = np.arange(taps) - taps // 2
    h = np.sinc(x * 2 * hz / rate) * np.hanning(taps)
    return (h / h.sum()).astype(np.float32)


def authenticated_index(path, expected):
    if not isinstance(expected, str) or len(expected) != 64:
        raise ValueError("Attach the index with the SHA-256 supplied by its trusted publisher.")
    # Hash and parse the same bounded snapshot so replacing the path cannot change the bytes.
    limit = 512 * 1024**2
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            raise ValueError("Expected a regular retrieval index of at most 512 MiB.")
        data = source.read(info.st_size)
        if len(data) != info.st_size or source.read(1) or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Retrieval index does not match the trusted SHA-256.")
    import faiss
    return faiss.deserialize_index(np.frombuffer(data, dtype=np.uint8))


class Converter:
    def __init__(self, cfg):
        from infer_pack.models import SynthesizerTrnMs256NSFsid, SynthesizerTrnMs768NSFsid

        torch.set_num_threads(3)
        spec = cfg["preset"]
        self.chunk = int(spec["chunk"]) * 48
        extra = int(spec["extra"]) * 48
        if not 2400 <= self.chunk <= 48000 or not 0 <= extra <= 72000:
            raise ValueError("Unsupported chunk or history length")
        self.overlap = min(1440, self.chunk // 2)
        self.search = 480
        self.fade_in = np.linspace(0, 1, self.overlap, dtype=np.float32)
        self.fade_out = 1 - self.fade_in
        self.energy_window = np.ones(self.overlap, np.float32)
        self.f0_threshold = np.array([0.3], np.float32)
        self.tail = None
        # Keep the newest sample aligned while giving the decoder complete 10 ms hops.
        window = extra + self.chunk + self.overlap + self.search
        self.history = np.zeros(math.ceil(window / 480) * 480, np.float32)
        self.pitch_scale = 2 ** (float(spec["pitch"]) / 12)
        threshold = 0 if spec["gate"] <= -120 else 10 ** (spec["gate"] / 20)
        self.down = lowpass(7600, 48000, 95)
        self.hp = lowpass(spec["highPass"], 48000, 511) if 20 < spec["highPass"] else None
        self.lp = lowpass(spec["lowPass"], 48000, 127) if spec["lowPass"] < 20000 else None
        from gate import GateHold
        hold_ms = spec.get("gateHoldMS", 0)
        if not 0 <= hold_ms <= 2000:
            raise ValueError("Gate hold must be between 0 and 2000 ms")
        self.gate = GateHold(threshold, hold_ms, self.overlap + self.search) if threshold else None
        model = Path(cfg["model"])
        if model.stat().st_size > 512 * 1024**2:
            raise ValueError("Expected an exported RVC inference checkpoint under 512 MiB")
        from packaging.version import Version
        if Version(torch.__version__) < Version("2.10.0"):
            raise ValueError("Install the updated MetalVoice2 voice runtime (PyTorch 2.10 or later).")
        weights = torch.load(model, map_location="cpu", weights_only=True)
        version = weights.get("version", "v1")
        if version not in ("v1", "v2") or not weights.get("f0", True):
            raise ValueError("This preview supports pitch-guided RVC v1 and v2 inference checkpoints")
        self.units = "units9" if version == "v1" else "unit12"
        architecture = list(weights["config"])
        self.rate = int(architecture[-1])
        if self.rate not in (32000, 40000, 48000) or math.prod(architecture[12]) != self.rate // 100:
            raise ValueError("Unsupported RVC decoder sample rate")
        speaker_count = weights["weight"]["emb_g.weight"].shape[0]
        if speaker_count < 1:
            raise ValueError("The checkpoint has no speaker embeddings")
        if cfg.get("reportSpeakerCount"):
            pcm.write(f"SPEAKERS {speaker_count}\n".encode("ascii"))
            pcm.flush()
        speaker_id = spec.get("speakerID", 0)
        if type(speaker_id) is not int or not 0 <= speaker_id < speaker_count:
            raise ValueError(f"Choose a speaker ID from 0 to {speaker_count - 1} for this model.")
        architecture[-3] = speaker_count
        decoder = SynthesizerTrnMs256NSFsid if version == "v1" else SynthesizerTrnMs768NSFsid
        self.net = decoder(*architecture, is_half=False)
        del self.net.enc_q
        missing, _ = self.net.load_state_dict(weights["weight"], strict=False)
        if missing:
            raise ValueError("Checkpoint is missing decoder weights: " + missing[0])
        self.net.eval()
        for layer in self.net.modules():
            if hasattr(layer, "weight_g"):
                torch.nn.utils.remove_weight_norm(layer)
        self.speaker = self.net.emb_g(torch.LongTensor([speaker_id])).unsqueeze(-1)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 3
        options.inter_op_num_threads = 1
        # The two ONNX sessions and the PyTorch decoder run one after another.
        options.add_session_config_entry("session.force_spinning_stop", "1")
        def session(file):
            return ort.InferenceSession(str(Path(cfg["assets"]) / file), sess_options=options,
                                        providers=["CPUExecutionProvider"])
        self.encoder = session("contentvec/contentvec-f.onnx")
        method = spec.get("f0", "rmvpe_onnx")
        self.harvest = None
        if method == "rmvpe_onnx":
            self.estimator = session("rmvpe/rmvpe_20231006.onnx")
        elif method == "harvest":
            from harvest import HarvestPitch
            self.harvest = HarvestPitch()
        else:
            raise ValueError("Unsupported F0 estimator: " + str(method))
        self.index = None
        self.mix = spec["indexStrength"] / 100
        if cfg.get("index") and self.mix:
            self.index = authenticated_index(cfg["index"], cfg.get("indexSHA256"))
            if self.index.d != (256 if version == "v1" else 768) or not self.index.ntotal:
                raise ValueError("Retrieval index does not match this voice")

    def convert(self, block, warmup=False):
        self.history[:-self.chunk] = self.history[self.chunk:]
        self.history[-self.chunk:] = block
        gate_window = None
        if self.gate is not None and not warmup:
            gate_window = self.gate.window(block)
            if not gate_window.any():
                self.tail = None
                return np.zeros_like(block)
        audio = np.convolve(self.history, self.down, mode="same")[::3]
        # Pad the encoder's receptive field so its final 20 ms feature is present.
        data = np.pad(audio, (0, 480), mode="reflect")[None]
        features = self.encoder.run([self.units], {"audio": data})[0][0]
        if self.index is not None:
            # IVF indexes can retrieve vectors by list offset even without a direct map.
            _, ids, vectors = self.index.search_and_reconstruct(np.ascontiguousarray(features), 1)
            matched = ids[:, 0] >= 0
            features[matched] += self.mix * (vectors[matched, 0] - features[matched])
        if self.harvest is not None:
            f0 = self.harvest.estimate(audio)
        else:
            f0 = self.estimator.run(["pitchf"], {"waveform":data, "threshold":self.f0_threshold})[0].reshape(-1)
        frames = len(audio) // 160
        f0 = f0[:frames] * self.pitch_scale
        features = np.repeat(features, 2, axis=0)[:frames]
        if len(f0) != frames or len(features) != frames:
            raise ValueError("Encoder frame count mismatch")
        mel = 1127 * np.log1p(f0 / 700)
        lo, hi = 1127 * np.log1p(np.array([50, 1100]) / 700)
        pitch = np.clip(np.rint((mel-lo)*254/(hi-lo)+1), 1, 255).astype(np.int64)
        mean, logs, mask = self.net.enc_p(torch.from_numpy(features[None]), torch.from_numpy(pitch[None]), torch.LongTensor([frames]))
        latent = self.net.flow((mean + torch.exp(logs)*torch.randn_like(mean)*0.66666)*mask,
                               mask, g=self.speaker, reverse=True)
        keep = math.ceil((self.chunk+self.overlap+self.search)/480) + 6
        wave = self.net.dec(latent[:, :, -keep:] * mask[:, :, -keep:], torch.from_numpy(f0[None, -keep:]), g=self.speaker)[0,0].numpy()
        if self.rate != 48000:
            wave = np.interp(np.arange(round(len(wave)*48000/self.rate))*self.rate/48000, np.arange(len(wave)), wave).astype(np.float32)
        if self.hp is not None:
            wave -= np.convolve(wave, self.hp, mode="same")
        if self.lp is not None:
            wave = np.convolve(wave, self.lp, mode="same")
        segment = wave[-(self.chunk + self.overlap + self.search):]
        offset = 0
        if self.tail is not None:
            candidates = segment[:self.overlap + self.search]
            energy = np.convolve(candidates**2, self.energy_window, "valid")
            offset = int(np.argmax(np.correlate(candidates, self.tail, "valid") / np.sqrt(energy+1e-8)))
        out = segment[offset:offset+self.chunk].copy()
        out[:self.overlap] *= self.fade_in
        if self.tail is not None:
            out[:self.overlap] += self.tail * self.fade_out
        self.tail = segment[offset+self.chunk:offset+self.chunk+self.overlap].copy()
        if gate_window is not None:
            out *= gate_window[offset:offset+self.chunk]
        return np.clip(out, -1, 1, out=out)


if __name__ == "__main__":
    ready = False
    try:
        config = json.loads(sys.argv[1])
        from isolation import confine
        confine(config)
        import numpy as np
        import onnxruntime as ort
        import torch
        with torch.inference_mode():
            converter = Converter(config)
            # Warm lazy kernels before hardware capture starts.
            converter.convert(np.full(converter.chunk, 0.01, np.float32), warmup=True)
            converter.history.fill(0)
            converter.tail = None
            pcm.write(b"OK\n")
            pcm.flush()
            ready = True
            size = converter.chunk * 4
            while True:
                raw = sys.stdin.buffer.read(size)
                if len(raw) != size:
                    break
                pcm.write(converter.convert(np.frombuffer(raw, dtype="<f4")).astype("<f4", copy=False))
                pcm.flush()
    except Exception as error:
        if not ready:
            pcm.write(("ERROR " + str(error).replace("\n", " ")[:350] + "\n").encode())
            pcm.flush()
        raise
