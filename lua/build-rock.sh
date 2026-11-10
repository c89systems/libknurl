#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 3 ]]; then
  printf '%s\n' 'usage: build-rock.sh TARGET CORE_SDK DEPENDENCY_SDK_ROOT' >&2
  exit 1
fi
knurl_source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
knurl_targets=$(bash "$knurl_source_root/scripts/lifecycle/cpkt-toolchains.sh" targets)
if [[ $'\n'$knurl_targets$'\n' != *$'\n'"$1"$'\n'* ]]; then
  printf 'lua-knurl: unsupported target: %s\n' "$1" >&2
  exit 1
fi
for prefix in "$2" "$3"; do
  if [[ $prefix != /* || ! -d $prefix ]]; then
    printf 'lua-knurl: provide absolute installed SDK paths; got: %s\n' "$prefix" >&2
    exit 1
  fi
done
knurl_build_dir="$knurl_source_root/build/$1/release"
if [[ $1 == x86_64-linux-gnu ]]; then
  knurl_build_dir="$knurl_source_root/build/release"
fi
cmake -S "$knurl_source_root/lua" -B "$knurl_build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$knurl_source_root/cmake/toolchains/selected.cmake" \
  -DKNURL_TARGET="$1" -DKNURL_CORE_PREFIX="$2" -DKNURL_SDK_ROOT="$3"
cmake --build "$knurl_build_dir" --target knurl_lua --parallel 1
cmake --install "$knurl_build_dir" --prefix "$knurl_source_root/build/rock-sdk"
cmake -E make_directory "$knurl_source_root/build/rock/knurl"
cmake -E copy_if_different "$knurl_source_root/build/rock-sdk/lib/lua/5.5/knurl/native.so" \
  "$knurl_source_root/build/rock/knurl/native.so"
