"""Run on macOS with the new voice runtime Python. Uses a harmless test file."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

code = Path(__file__).resolve().parents[2] / "Resources/RVC"
if len(sys.argv) == 1:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder).resolve()
        for name in ("selected.pth", "unrelated"):
            (root / name).write_bytes(b"fixture")
        child = subprocess.run([sys.executable, "-I", "-B", "-S", __file__, str(root)], capture_output=True)
        assert child.returncode == 0, child.stderr.decode(errors="replace")
        print(child.stdout.decode().strip())
else:
    root = Path(sys.argv[1])
    sys.path.insert(0, str(code))
    from isolation import confine
    confine(dict(model=root / "selected.pth", assets=root))
    assert (root / "selected.pth").read_bytes() == b"fixture"
    actions = [lambda: (root / "unrelated").read_bytes(),
               lambda: (root / "new-file").write_bytes(b"fixture"),
               lambda: socket.socket(socket.AF_INET, socket.SOCK_STREAM).bind(("127.0.0.1", 0)),
               lambda: subprocess.run(["/usr/bin/true"], check=True)]
    for action in actions:
        try:
            action()
        except PermissionError:
            continue
        raise AssertionError("Worker sandbox allowed an unauthorized operation")
    print("PASS isolated worker reads selected input and denies unrelated reads, writes, sockets and subprocesses")
