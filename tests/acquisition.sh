#!/usr/bin/env bash
set -euo pipefail
root=$1 work=$2
cmake "-DROTMOS_CHECK_PATH=$work" -P "$root/cmake/OwnedPaths.cmake"
source "$root/scripts/acquire-archive.sh"
rm -rf -- "$work"
mkdir -p "$work/input" "$work/global"
printf 'small archive fixture\n' >"$work/input/data"
tar -czf "$work/archive.tar.gz" -C "$work/input" data
: >"$work/requests"
python3 "$root/tests/fixtures/archive-origin.py" "$work" >"$work/origin.log" 2>&1 &
origin=$!
trap 'kill "$origin" 2>/dev/null || :; wait "$origin" 2>/dev/null || :' EXIT
for ((attempt = 0; attempt < 100; attempt++)); do
  [[ ! -s "$work/port" ]] || break
  sleep 0.05
done
[[ -s "$work/port" ]] || { cat "$work/origin.log" >&2; exit 1; }
url="http://127.0.0.1:$(cat "$work/port")"
digest=$(cpkt_cached_archive_sha256 "$work/archive.tar.gz")
cache="$work/cache"
destination="$cache/archives/sha256/$digest/archive.tar.gz"
requests() {
  local count
  count=$(wc -l <"$work/requests")
  [[ $count == "$1" ]] || { printf 'Expected %s requests, got %s\n' "$1" "$count" >&2; exit 1; }
}
# Expected failures run in a separate Bash so the surrounding `if` does not
# disable errexit inside the acquisition helper.
try_acquire() {
  bash -euo pipefail -c 'source "$1/scripts/acquire-archive.sh"; shift; rotmos_acquire_archive "$@"' \
    -- "$root" "$@"
}
acquire() { try_acquire "$1" "$2" "$cache" "$work/global"; }
requests 0
acquire "$url/archive.tar.gz" "$digest"
cmp "$work/archive.tar.gz" "$destination"
requests 1
acquire "$url/archive.tar.gz" "$digest"
requests 1
mv "$destination" "${destination%/*}/renamed.tar.gz"
acquire "$url/archive.tar.gz" "$digest"
requests 1
rm "${destination%/*}/renamed.tar.gz"
printf 'corrupt\n' >"$destination"
acquire "$url/archive.tar.gz" "$digest"
cmp "$work/archive.tar.gz" "$destination"
requests 2
mkdir -p "$work/global/archives/sha256/$digest"
cp "$destination" "$work/global/archives/sha256/$digest/archive.tar.gz"
try_acquire "$url/archive.tar.gz" "$digest" "$work/global-hit" "$work/global"
requests 2
bad=0000000000000000000000000000000000000000000000000000000000000000
reject() {
  if acquire "$url/$1" "$bad" >"$work/rejected.log" 2>&1; then
    printf 'Acquisition unexpectedly accepted %s\n' "$1" >&2; exit 1
  fi
  [[ -z $(find "$cache/archives/sha256/$bad" -type f -print -quit) ]] || {
    printf 'Failed acquisition left archive or temporary bytes\n' >&2; exit 1;
  }
}
reject archive.tar.gz
requests 3
reject missing.tar.gz
requests 4
reject truncated.tar.gz
# Read-only global inputs survive success and checksum failure unchanged.
printf 'corrupt global\n' >"$work/global/archives/sha256/$digest/archive.tar.gz"
before=$(wc -l <"$work/requests")
if try_acquire "$url/archive.tar.gz" "$digest" "$work/bad-global" "$work/global" \
    >"$work/rejected.log" 2>&1; then
  printf 'Corrupt global archive was accepted\n' >&2; exit 1
fi
[[ $(cat "$work/global/archives/sha256/$digest/archive.tar.gz") == 'corrupt global' ]]
[[ -z $(find "$work/bad-global/archives/sha256/$digest" -type f -print -quit) ]]
requests "$before"
printf 'Archive acquisition: miss, zero-request hits, corruption and failure cleanup passed\n'
