#!/usr/bin/env bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
source "$script_dir/lifecycle/require-host-bash.sh"
source "$script_dir/lifecycle/cpkt-archive-cache.sh"
target=${1:-x86_64-linux-gnu}
lua_sources=${2:-}
case "$lua_sources" in ''|lua) ;; *) printf 'Usage: deps.sh TARGET [lua]\n' >&2; exit 2 ;; esac
case "$target" in
  x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin) ;;
  *) printf 'Unknown SDK target: %s\n' "$target" >&2; exit 2 ;;
esac
sdk_root="$repo_root/.cache/sdk/$target"
archive_cache=${CPKT_DEPENDENCY_CACHE:-$repo_root/.cache/deps}
for owned in "$sdk_root" "$archive_cache"; do
  "${CMAKE:-cmake}" "-DROTMOS_CHECK_PATH=$owned" -P "$repo_root/cmake/OwnedPaths.cmake"
done
mkdir -p "$sdk_root" "$archive_cache/archives/sha256" "$archive_cache/locks"

acquire() (
  local url=$1 digest=$2 name=${1##*/} destination temporary global_cache candidate actual
  destination="$archive_cache/archives/sha256/$digest/$name"
  mkdir -p "${destination%/*}"
  exec 9>"$archive_cache/locks/$digest.lock"
  flock -w 600 9
  cpkt_restore_cached_archive "$destination" "$digest"
  if [[ ! -f "$destination" ]]; then
    temporary=$(mktemp "${destination%/*}/.download.XXXXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
    global_cache="${XDG_CACHE_HOME:-${HOME:?HOME is required}/.cache}/cpkt/deps"
    candidate="$global_cache/archives/sha256/$digest/$name"
    if [[ -f "$candidate" ]]; then
      cp -- "$candidate" "$temporary"
    else
      curl --fail --location --retry 3 --connect-timeout 20 --output "$temporary" "$url"
    fi
    actual=$(cpkt_cached_archive_sha256 "$temporary")
    [[ "$actual" == "$digest" ]] || { printf 'Checksum mismatch: %s\n' "$name" >&2; exit 1; }
    mv -- "$temporary" "$destination"
  fi
)

staging=
manifest=$(mktemp "$sdk_root/.manifest.XXXXXXXX")
trap 'rm -f -- "$manifest"; if [[ -n "$staging" ]]; then rm -rf -- "$staging"; fi' EXIT
printf 'target=%s\n' "$target" >"$manifest"
while IFS=$'\t' read -r kind name version pin_target digest url; do
  [[ "$kind" != \#* && -n "$kind" ]] || continue
  if [[ "$kind" == sdk && "$pin_target" == "$target" ]]; then
    prefix="$sdk_root/$name"
    marker="$sdk_root/.prepared-$name"
  elif [[ "$kind" == lua && "$pin_target" == all && "$lua_sources" == lua ]]; then
    prefix="$archive_cache/sources/$name-lua-$version"
    marker="$archive_cache/sources/.prepared-$name-lua-$version"
    mkdir -p "$archive_cache/sources"
  else
    continue
  fi
  [[ "$name" =~ ^[a-z0-9-]+$ && "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$digest" =~ ^[a-f0-9]{64}$ && "$url" == https://github.com/c89systems/* ]] || {
    printf 'Invalid dependency pin: %s\n' "$name" >&2; exit 2;
  }
  acquire "$url" "$digest"
  archive="$archive_cache/archives/sha256/$digest/${url##*/}"
  if [[ ! -d "$prefix" || ! -f "$marker" || "$(cat "$marker")" != "$digest" ]]; then
    staging=$(mktemp -d "$sdk_root/.extract-$name.XXXXXXXX")
    tar -tzf "$archive" >"$staging/.entries"
    while IFS= read -r entry; do
      case "$entry" in /*|../*|*/../*|*/..) printf 'Unsafe SDK archive path: %s\n' "$entry" >&2; exit 1;; esac
    done <"$staging/.entries"
    rm -f -- "$staging/.entries"
    # Reproducible archives carry old mtimes. New SDK bytes must invalidate
    # existing CMake/Ninja header and linker dependencies after an upgrade.
    tar -xmzf "$archive" -C "$staging" --strip-components=1
    rm -rf -- "$prefix"
    mv -- "$staging" "$prefix"
    staging=
    printf '%s\n' "$digest" >"$marker"
  fi
  if [[ "$kind" == sdk ]]; then
    printf '%s_version=%s\n%s_sha256=%s\n' "$name" "$version" "$name" "$digest" >>"$manifest"
  fi
done <"$script_dir/dependency-pins.tsv"
if [[ ! -f "$sdk_root/manifest.txt" ]] || ! cmp -s "$manifest" "$sdk_root/manifest.txt"; then
  mv -- "$manifest" "$sdk_root/manifest.txt"
fi
printf 'Prepared SDKs: %s\n' "$sdk_root"
