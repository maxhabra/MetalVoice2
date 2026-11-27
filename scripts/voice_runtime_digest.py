"""Build-time counterpart of VoiceRuntime.treeDigest: bind paths, links and file bytes."""
import hashlib
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
tree = hashlib.sha256()
for path in sorted(root.rglob("*"), key=lambda p: os.fsencode(p.relative_to(root))):
    if path.is_symlink():
        target = os.readlink(path)
        if os.path.isabs(target) or root not in path.resolve().parents:
            raise ValueError(f"External runtime link: {path}")
        kind, digest = b"L", hashlib.sha256(os.fsencode(target))
    elif path.is_file():
        kind, digest = b"F", hashlib.sha256(path.read_bytes())
    elif path.is_dir():
        continue
    else:
        raise ValueError(f"Unexpected file type: {path}")
    tree.update(os.fsencode(path.relative_to(root)) + b"\0" + kind + digest.digest())
print(tree.hexdigest())
