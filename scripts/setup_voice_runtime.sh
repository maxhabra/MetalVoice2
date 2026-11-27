#!/bin/bash
# Build the optional, administrator-installed runtime without executing pip as root.
set -euo pipefail
if [[ "$(uname -m)" != arm64 || "$(id -u)" == 0 ]]; then
    echo "Run this script as your normal user on Apple Silicon." >&2
    exit 1
fi
here="$(cd "$(dirname "$0")" && pwd)"
app="${1:-$here/MetalVoice2.app}"
if [[ $# == 0 && ! -d "$app" ]]; then app="$here/../build/MetalVoice2.app"; fi
if [[ ! -d "$app/Contents/Resources/RVC" ]]; then
    echo "Pass the path to the built MetalVoice2.app as the first argument." >&2
    exit 1
fi
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
root="$stage/payload"
mkdir -p "$root/Contents/Resources" "$root/Contents/Frameworks"
archive='https://github.com/astral-sh/python-build-standalone/releases/download/20261124/cpython-3.11.18%2B20261124-aarch64-apple-darwin-install_only_stripped.tar.gz'
curl --fail --location --silent --show-error "$archive" -o "$stage/python.tar.gz"
(cd "$stage" && echo 'e9840948169972436f80241773458cdb95923a639f652c01bdc3f1066e45f113  python.tar.gz' | shasum -a 256 -c -)
tar -xzf "$stage/python.tar.gz" -C "$root"
ditto --norsrc --noextattr "$app/Contents/Resources/RVC" "$root/Contents/Resources/RVC"
cp -X "$app/Contents/Frameworks/libWorldHarvest.dylib" "$root/Contents/Frameworks/"
"$root/python/bin/python3.11" -I -B -m pip --isolated download --index-url https://pypi.org/simple \
    --only-binary=:all: --no-deps --require-hashes --disable-pip-version-check --dest "$stage/wheels" \
    -r "$root/Contents/Resources/RVC/runtime-requirements.txt"
# The app authenticates this exact tree. Unpack the locked wheels verbatim:
# pip install would generate machine-specific scripts and bytecode.
for wheel in "$stage/wheels/"*.whl; do
    /usr/bin/unzip -oq "$wheel" -d "$root/python/lib/python3.11/site-packages"
done
# Both wheels use the same OpenMP ABI. Loading both copies aborts indexed conversion.
ln -sf ../../torch/lib/libomp.dylib "$root/python/lib/python3.11/site-packages/faiss/.dylibs/libomp.dylib"
chmod -RN "$root"
chmod -R go-w "$root"
destination="${2:-$PWD/MetalVoice2-VoiceRuntime.pkg}"
pkgbuild --root "$root" --ownership recommended --install-location /Library/MetalVoice2/RVC-1 \
    --identifier app.metalvoice2.voice-runtime --version 1.0 "$destination"
echo "Open $destination and complete macOS Installer to enable voice conversion."
