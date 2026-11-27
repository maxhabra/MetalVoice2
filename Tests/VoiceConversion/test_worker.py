"""Run with the RVC runtime Python after building the app: test_worker.py MODEL.pth MODULES_DIR."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import numpy as np


def run_worker(worker, model, assets, method, speaker_id, audio, gate=-120):
    preset = dict(chunk=50, extra=0, gate=gate, pitch=0, highPass=20, lowPass=20000,
                  indexStrength=0, f0=method)
    if speaker_id is not None:
        preset["speakerID"] = speaker_id
    config = dict(model=str(model.resolve()), assets=str(assets.resolve()), preset=preset, reportSpeakerCount=True)
    proc = subprocess.run([sys.executable, "-I", "-B", "-S", str(worker), json.dumps(config)],
                          input=audio.tobytes(), capture_output=True, timeout=90)
    metadata, _, response = proc.stdout.partition(b"\n")
    assert metadata.startswith(b"SPEAKERS "), proc.stderr.decode(errors="replace")
    status, _, pcm = response.partition(b"\n")
    count = int(metadata.split()[1])
    output = np.frombuffer(pcm, dtype="<f4")
    if speaker_id is None or speaker_id < count:
        assert proc.returncode == 0 and status == b"OK", proc.stderr.decode(errors="replace")
        assert output.size == audio.size and np.isfinite(output).all() and np.any(output)
    else:
        assert proc.returncode != 0 and status.startswith(b"ERROR Choose a speaker ID") and not pcm
    return count, output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    worker = repo / "build/MetalVoice2.app/Contents/Resources/RVC/worker.py"
    # Consecutive voiced chunks preserve the crossfade tail between calls.
    samples = np.arange(32 * 2400) / 48000
    signal = (.2 * np.sin(2 * np.pi * (140 * samples + 90 * samples**2))).astype("<f4")
    count, _ = run_worker(worker, args.model, args.assets, "rmvpe_onnx", None, signal)
    run_worker(worker, args.model, args.assets, "harvest", count - 1, signal)
    run_worker(worker, args.model, args.assets, "harvest", count, signal)
    print(f"PASS minimum chunk with zero history, legacy preset and speaker range 0–{count - 1}")

    # A closed chunk must finish the aligned release before the worker skips inference.
    tone = signal[:2400]
    quiet = np.zeros_like(tone)
    sequence = np.concatenate([quiet, tone, tone, quiet, quiet, tone, tone])
    _, gated = run_worker(worker, args.model, args.assets, "harvest", 0, sequence, gate=-40)
    chunks = gated.reshape(-1, 2400)
    assert not chunks[0].any() and chunks[2].any()
    assert chunks[3][:1200].any() and not chunks[3][1680+96:].any()
    assert not chunks[4].any() and not chunks[5][:1200].any() and chunks[6].any()
    print("PASS default zero hold closes and reopens on the decoder timeline")
