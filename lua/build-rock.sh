#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 3 ]]; then
  printf '%s\n' 'usage: build-rock.sh TARGET CORE_SDK DEPENDENCY_SDK_ROOT' >&2
  exit 1
fi
rotmos_source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
rotmos_targets=$(bash "$rotmos_source_root/scripts/lifecycle/cpkt-toolchains.sh" targets)
if [[ $'\n'$rotmos_targets$'\n' != *$'\n'"$1"$'\n'* ]]; then
  printf 'lua-rotmos: unsupported target: %s\n' "$1" >&2
  exit 1
fi
for prefix in "$2" "$3"; do
  if [[ $prefix != /* || ! -d $prefix ]]; then
    printf 'lua-rotmos: provide absolute installed SDK paths; got: %s\n' "$prefix" >&2
    exit 1
  fi
done
rotmos_build_dir="$rotmos_source_root/build/$1/release"
if [[ $1 == x86_64-linux-gnu ]]; then
  rotmos_build_dir="$rotmos_source_root/build/release"
fi
cmake -S "$rotmos_source_root/lua" -B "$rotmos_build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$rotmos_source_root/cmake/toolchains/selected.cmake" \
  -DROTMOS_TARGET="$1" -DROTMOS_CORE_PREFIX="$2" -DROTMOS_SDK_ROOT="$3"
cmake --build "$rotmos_build_dir" --target rotmos_lua --parallel 1
cmake --install "$rotmos_build_dir" --prefix "$rotmos_source_root/build/rock-sdk"
cmake -E make_directory "$rotmos_source_root/build/rock/rotmos"
cmake -E copy_if_different "$rotmos_source_root/build/rock-sdk/lib/lua/5.5/rotmos/native.so" \
  "$rotmos_source_root/build/rock/rotmos/native.so"
