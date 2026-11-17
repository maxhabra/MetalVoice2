#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 scripts/verify_localvqe.py
if [[ ! -x build/localvqe-env/bin/cmake || ! -x build/localvqe-env/bin/ninja ]]; then
    python3 -m venv build/localvqe-env
    build/localvqe-env/bin/python -m pip install --disable-pip-version-check 'cmake==3.31.10' 'ninja==1.13.0'
fi
build/localvqe-env/bin/cmake -S ThirdParty/LocalVQE -B build/localvqe-native -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$PWD/build/localvqe-env/bin/ninja" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
build/localvqe-env/bin/cmake --build build/localvqe-native --target mv_localvqe_api localvqe-regression -j6
archives=(build/localvqe-native/libmv_localvqe_api.a build/localvqe-native/ggml/src/libggml.a \
    build/localvqe-native/ggml/src/libggml-base.a build/localvqe-native/ggml/src/libggml-cpu.a)
relink=false
for archive in "${archives[@]}"; do
    if [[ "$archive" -nt build/localvqe-native/libmv_localvqe.a ]]; then relink=true; fi
done
if [[ ! -f build/localvqe-native/libmv_localvqe.a || "$relink" == true ]]; then
    libtool -static -o build/localvqe-native/libmv_localvqe.a "${archives[@]}"
fi
