#!/bin/bash
# Invoked on first voice use by MetalVoice2. Only the chosen interpreter path goes to stdout.
set -euo pipefail
umask 077
support="$1"
runtime="$support/engine-1"
requirements="$(dirname "$0")/runtime-requirements.txt"
if [[ -x "$runtime/bin/python3.11" ]]; then
    printf '%s\n' "$runtime/bin/python3.11"
    exit 0
fi
legacy="$support/venv/bin/python3"
# Reuse a compatible beta environment without installing into it. Avoid legacy .pth hooks.
if [[ -x "$legacy" ]] && "$legacy" -IBS - "$requirements" <<'PY'
import importlib.metadata as metadata
from pathlib import Path
import sys
if sys.version_info[:2] != (3, 11):
    sys.exit(1)
site = Path(sys.executable).parent.parent / "lib/python3.11/site-packages"
sys.path.append(str(site))
for line in Path(sys.argv[1]).read_text().splitlines():
    if line and not line.startswith("#"):
        name, version = line.split()[0].split("==")
        if metadata.version(name) != version:
            sys.exit(1)
sys.exit(0 if (site/"torch/lib/libomp.dylib").samefile(site/"faiss/.dylibs/libomp.dylib") else 1)
PY
then
    printf '%s\n' "$legacy"
    exit 0
fi

stage="$(mktemp -d "$support/.prepare.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
archive='https://github.com/astral-sh/python-build-standalone/releases/download/20261124/cpython-3.11.18%2B20261124-aarch64-apple-darwin-install_only_stripped.tar.gz'
curl --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 300 "$archive" -o "$stage/python.tar.gz"
(cd "$stage" && echo 'e9840948169972436f80241773458cdb95923a639f652c01bdc3f1066e45f113  python.tar.gz' | shasum -a 256 -c -) >&2
tar -xzf "$stage/python.tar.gz" -C "$stage"
python="$stage/python/bin/python3.11"
"$python" -IB -m pip --isolated download --index-url https://pypi.org/simple --no-cache-dir \
    --only-binary=:all: --no-deps --require-hashes --disable-pip-version-check --dest "$stage/wheels" -r "$requirements" >&2
site="$stage/python/lib/python3.11/site-packages"
for wheel in "$stage/wheels/"*.whl; do
    /usr/bin/unzip -oq "$wheel" -d "$site"
done
# Torch and FAISS share one OpenMP library when both are active.
ln -sf ../../torch/lib/libomp.dylib "$site/faiss/.dylibs/libomp.dylib"
"$python" -IB - "$stage/python" "$runtime" <<'PY'
import os
import sys
from pathlib import Path
try:
    os.rename(sys.argv[1], sys.argv[2])
except OSError:
    # Another copy of the app may have completed the same atomic install.
    if not os.access(Path(sys.argv[2]) / "bin/python3.11", os.X_OK):
        raise
PY
printf '%s\n' "$runtime/bin/python3.11"
