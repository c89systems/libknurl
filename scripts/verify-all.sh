#!/usr/bin/env bash
# One foreground operation owns the checkout. Build producers once, then check.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$root"
[[ $# == 3 && $1 =~ ^(debug|release)$ && $2 =~ ^[1-9][0-9]*$ ]] || exit 2
config=$1 jobs=$2 candidate=$3
source "$root/scripts/lifecycle/require-host-bash.sh"
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || {
  printf 'The full local matrix requires x86_64 Linux with approved osxcross\n' >&2; exit 1;
}
for tool in valgrind qemu-aarch64 qemu-arm; do
  command -v "$tool" >/dev/null || { printf 'Required check tool missing: %s\n' "$tool" >&2; exit 1; }
done
"$root/scripts/lifecycle/cpkt-aflpp.sh" discover >/dev/null
make --no-print-directory lua-all CONFIG="$config" JOBS="$jobs" CANDIDATE_VERSION="$candidate"
targets=$(scripts/lifecycle/cpkt-toolchains.sh targets)
for target in $targets; do
  binary="$root/build/$target/$config"
  [[ $target != x86_64-linux-gnu ]] || binary="$root/build/$config"
  case "$target" in
    x86_64-linux-gnu)
      ctest --test-dir "$binary" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
      ctest --test-dir "$binary/lua" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error ;;
    x86_64-linux-musl)
      ctest --test-dir "$binary" -E '^(kore[.]|lifecycle[.])' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
      ctest --test-dir "$binary/lua" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error ;;
    *-linux-*)
      cmake -S cmake/checks -B "$binary/checks/qemu" "-DROTMOS_BUILD=$binary" -DCHECK_KIND=qemu
      ctest --test-dir "$binary/checks/qemu" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error ;;
  esac
  # Native Core's sdk-package test already verifies its actual SDK archive.
  # Cross targets need that artifact proof separately from the small smoke.
  if [[ $target != x86_64-linux-* ]]; then
    cmake "-DBUILD_DIR=$binary" -P cmake/Package.cmake
    cmake "-DBUILD_DIR=$binary" -P cmake/VerifySDK.cmake
  fi
done
binary="$root/build/$config"
cmake -S cmake/checks -B "$binary/checks/memory" "-DROTMOS_BUILD=$binary" -DCHECK_KIND=memory
ctest --test-dir "$binary/checks/memory" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
if [[ $config != release ]]; then
  make --no-print-directory lua TARGET=x86_64-linux-gnu CONFIG=release JOBS="$jobs" CANDIDATE_VERSION="$candidate"
fi
binary="$root/build/release"
cmake -S tests/fuzz -B "$binary/fuzz" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  "-DCMAKE_TOOLCHAIN_FILE=$root/cmake/toolchains/selected.cmake" -DROTMOS_TARGET=x86_64-linux-gnu -DROTMOS_AFL=ON \
  "-DROTMOS_CORE_PREFIX=$binary/lua-core-sdk" "-DROTMOS_SDK_ROOT=$root/.cache/sdk/x86_64-linux-gnu" "-DROTMOS_CANDIDATE_VERSION=$candidate"
cmake --build "$binary/fuzz" --parallel "$jobs"
ctest --test-dir "$binary/fuzz" --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
bash scripts/fuzz.sh "$binary/fuzz" smoke 10
make --no-print-directory format-check
