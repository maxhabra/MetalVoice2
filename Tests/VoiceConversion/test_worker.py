"""Run with the RVC venv after building the app: test_worker.py MODEL.pth MODULES_DIR."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import numpy as np


def run_worker(worker, model, assets, method, speaker_id, audio):
    preset = dict(chunk=50, extra=0, gate=-120, pitch=0, highPass=20, lowPass=20000,
                  indexStrength=0, f0=method)
    if speaker_id is not None:
        preset["speakerID"] = speaker_id
    config = dict(model=str(model), assets=str(assets), preset=preset, reportSpeakerCount=True)
    proc = subprocess.run([sys.executable, "-I", "-B", str(worker), json.dumps(config)],
                          input=audio.tobytes(), capture_output=True, timeout=90)
    metadata, _, response = proc.stdout.partition(b"\n")
    assert metadata.startswith(b"SPEAKERS "), proc.stderr.decode(errors="replace")
    status, _, pcm = response.partition(b"\n")
    count = int(metadata.split()[1])
    if speaker_id is None or speaker_id < count:
        assert proc.returncode == 0 and status == b"OK", proc.stderr.decode(errors="replace")
        output = np.frombuffer(pcm, dtype="<f4")
        assert output.size == audio.size and np.isfinite(output).all() and np.any(output)
    else:
        assert proc.returncode != 0 and status.startswith(b"ERROR Choose a speaker ID") and not pcm
    return count


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
    count = run_worker(worker, args.model, args.assets, "rmvpe_onnx", None, signal)
    run_worker(worker, args.model, args.assets, "harvest", count - 1, signal)
    run_worker(worker, args.model, args.assets, "harvest", count, signal)
    print(f"PASS minimum chunk with zero history, legacy preset and speaker range 0–{count - 1}")
