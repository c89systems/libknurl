#!/usr/bin/env bash
set -euo pipefail
root=$1 work=$2
cmake "-DKNURL_CHECK_PATH=$work" -P "$root/cmake/OwnedPaths.cmake"
source "$root/scripts/lifecycle/cpkt-archive-cache.sh"
rm -rf -- "$work"
owner="$work/owner"
mkdir -p "$owner/scripts/lifecycle" "$owner/cmake" "$work/borrowed" "$work/fixture/include"
cp "$root/scripts/deps.sh" "$root/scripts/acquire-archive.sh" "$owner/scripts/"
cp "$root/scripts/lifecycle/require-host-bash.sh" "$root/scripts/lifecycle/cpkt-archive-cache.sh" "$owner/scripts/lifecycle/"
cp "$root/cmake/OwnedPaths.cmake" "$owner/cmake/"
printf 'fixture header\n' >"$work/fixture/include/fixture.h"
chmod 664 "$work/fixture/include/fixture.h"
chmod 775 "$work/fixture/include"
tar -czf "$work/fixture.tar.gz" -C "$work" fixture
digest=$(cpkt_cached_archive_sha256 "$work/fixture.tar.gz")
archive="$owner/.cache/deps/archives/sha256/$digest"
mkdir -p "$archive"
cp "$work/fixture.tar.gz" "$archive/fixture.tar.gz"
printf 'sdk\tfixture\t1.2.3\tx86_64-linux-gnu\t%s\thttps://github.com/c89systems/fixture/fixture.tar.gz\n' \
  "$digest" >"$owner/scripts/dependency-pins.tsv"
deps() { CPKT_DEPENDENCY_CACHE="$owner/.cache/deps" bash "$owner/scripts/deps.sh" x86_64-linux-gnu >"$work/deps.log" 2>&1; }
(umask 077; deps)
sdk="$owner/.cache/sdk/x86_64-linux-gnu"
cmp "$work/fixture/include/fixture.h" "$sdk/fixture/include/fixture.h"
check_mode() {
  local actual
  if [[ $(uname -s) == Darwin ]]; then actual=$(stat -f %Lp "$1")
  else actual=$(stat -c %a "$1"); fi
  [[ "$actual" == "$2" ]] || {
    printf 'SDK extraction changed archived mode: %s (%s != %s)\n' "$1" "$actual" "$2" >&2
    exit 1
  }
}
check_mode "$sdk/fixture/include/fixture.h" 664
check_mode "$sdk/fixture/include" 775
printf 'preserve warm output\n' >"$sdk/fixture/warm"
deps
[[ -f "$sdk/fixture/warm" ]]
cp "$sdk/manifest.txt" "$work/manifest"
rm -rf -- "$sdk/fixture"
printf 'borrowed SDK\n' >"$work/borrowed/sentinel"
ln -s "$work/borrowed" "$sdk/fixture"
reject_borrowed() {
  if deps; then
    printf 'Dependency preparation accepted a borrowed SDK prefix\n' >&2; exit 1
  fi
  if [[ $(<"$work/deps.log") != *symlink* ]]; then cat "$work/deps.log" >&2; exit 1; fi
  [[ -L "$sdk/fixture" && $(cat "$work/borrowed/sentinel") == 'borrowed SDK' ]]
  cmp "$sdk/manifest.txt" "$work/manifest"
}
# Both reuse and re-preparation must reject the borrowed prefix, without repair.
reject_borrowed
rm "$sdk/.prepared-fixture"
reject_borrowed
# Direct Core configuration must reject a borrowed component too.
ln -s "$work/borrowed" "$sdk/cpkt"
cat >"$work/check.cmake" <<'CMAKE'
include("${OWNER}/cmake/OwnedPaths.cmake")
include("${KNURL_ROOT}/cmake/Dependencies.cmake")
CMAKE
if cmake "-DOWNER=$owner" "-DKNURL_ROOT=$root" "-DKNURL_SDK_ROOT=$sdk" \
    -DKNURL_TARGET=x86_64-linux-gnu -P "$work/check.cmake" >"$work/configure.log" 2>&1; then
  printf 'Dependency discovery accepted a borrowed component\n' >&2; exit 1
fi
if [[ $(<"$work/configure.log") != *symlink* ]]; then cat "$work/configure.log" >&2; exit 1; fi
printf 'Dependency prefixes: cold/warm owned preparation and borrowed-prefix rejection passed\n'
