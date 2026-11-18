#!/usr/bin/env python3
"""Compare the native denoisers on a fixed speaker/noise/SNR-balanced VoiceBank-DEMAND subset."""
import argparse
import csv
import hashlib
import io
from collections import defaultdict
from pathlib import Path
import subprocess
import tempfile
from zipfile import ZipFile

import numpy as np
from pesq import pesq
from pystoi import stoi
from scipy.io import wavfile
from scipy.signal import resample_poly


def select_ids(log_archive):
    groups = defaultdict(list)
    with ZipFile(log_archive) as archive:
        for line in archive.read("log_testset.txt").decode().splitlines():
            if line.strip():
                clip, noise, snr = line.split()
                groups[(clip.split("_")[0], noise, float(snr))].append(clip)
    if len(groups) != 40:
        raise ValueError(f"Expected two speakers × five noises × four SNRs, found {len(groups)} cells")
    # Pick once, independently of contents or results. Strings hash identically across Python versions.
    return [min(groups[group], key=lambda name: hashlib.sha256(name.encode()).digest())
            for group in sorted(groups)]


def waveform(archive, kind, clip):
    data = archive.read(f"{kind}_testset_wav/{clip}.wav")
    rate, samples = wavfile.read(io.BytesIO(data))
    if rate != 48000 or samples.ndim != 1 or samples.dtype != np.int16:
        raise ValueError(f"Expected original 48 kHz mono 16-bit PCM for {clip}")
    return samples.astype(np.float32) / 32768.0


def evaluate(args):
    ids = select_ids(args.logfiles)
    references = {}
    rows = []
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        inputs, outputs = root / "inputs", root / "outputs"
        inputs.mkdir()
        with ZipFile(args.clean) as clean, ZipFile(args.noisy) as noisy:
            for clip in ids:
                reference, mixture = waveform(clean, "clean", clip), waveform(noisy, "noisy", clip)
                if reference.size != mixture.size:
                    raise ValueError(f"Clean/noisy length mismatch for {clip}")
                references[clip] = (resample_poly(reference, 1, 3), mixture.size)
                mixture.tofile(inputs / f"{clip}.f32")
        subprocess.run([args.renderer.resolve(), inputs, outputs, args.resources.resolve()], check=True)
        for directory in sorted(outputs.iterdir()):
            scores = []
            for clip in ids:
                reference, n = references[clip]
                audio = np.fromfile(directory / f"{clip}.f32", dtype=np.float32)
                if audio.size != n or not np.isfinite(audio).all():
                    raise ValueError(f"Invalid render for {clip}")
                degraded = resample_poly(audio, 1, 3)
                quality = pesq(16000, reference, degraded, "wb")
                intelligibility = stoi(reference, degraded, 16000, extended=False)
                if not np.isfinite([quality, intelligibility]).all():
                    raise ValueError(f"Invalid scores for {clip}")
                rows.append((directory.name, clip, quality, intelligibility))
                scores.append((quality, intelligibility))
            quality, intelligibility = np.mean(scores, axis=0)
            print(f"{directory.name}: PESQ {quality:.4f}, STOI {intelligibility:.4f} (n={len(scores)})", flush=True)
    with args.csv.open("w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["mode", "clip", "pesq_wb", "stoi"])
        writer.writerows(rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("renderer", "resources", "clean", "noisy", "logfiles", "csv"):
        parser.add_argument("--" + name, type=Path, required=True)
    evaluate(parser.parse_args())
