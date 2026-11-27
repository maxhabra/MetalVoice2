"""Exercise the index trust boundary with an ordinary, generated FAISS index."""
import hashlib
from pathlib import Path
import runpy
import sys
import tempfile

import torch
import faiss
import numpy as np

worker = Path(__file__).resolve().parents[2] / "Resources/RVC/worker.py"
load_index = runpy.run_path(str(worker), init_globals={"np": np})["authenticated_index"]
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder) / "fixture.index"
    index = faiss.IndexFlatL2(8)
    index.add(np.ones((2, 8), np.float32))
    faiss.write_index(index, str(path))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    # Exercise both libraries' OpenMP kernels in the same process as voice conversion does.
    torch.set_num_threads(2)
    torch.ones(512, 512).sum()
    loaded = load_index(path, digest)
    assert loaded.ntotal == 2
    distances, _ = loaded.search(np.ones((32, 8), np.float32), 1)
    assert (distances == 0).all()
    index.add(np.zeros((1, 8), np.float32))
    faiss.write_index(index, str(path))
    for expected in (None, digest):
        try:
            load_index(path, expected)
        except ValueError:
            continue
        raise AssertionError("Expected rejection before index deserialization")
print("PASS index authentication and changed-file rejection", file=sys.stderr)
