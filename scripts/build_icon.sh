#!/bin/bash
# Regenerate the Finder icon after changing the 1024 px master.
set -euo pipefail
cd "$(dirname "$0")/.."
staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/metalvoice2-icon.XXXXXX")"
trap 'rm -rf "$staging_dir"' EXIT
iconset="$staging_dir/AppIcon.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    for scale in 1 2; do
        pixels=$((size * scale))
        suffix=""
        if [[ "$scale" == 2 ]]; then suffix="@2x"; fi
        sips -z "$pixels" "$pixels" Resources/Brand/AppIcon.png \
            --out "$iconset/icon_${size}x${size}${suffix}.png" >/dev/null
    done
done
iconutil -c icns "$iconset" -o "$staging_dir/AppIcon.icns"
cp -X "$staging_dir/AppIcon.icns" Resources/Brand/AppIcon.icns
