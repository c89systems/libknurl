#!/usr/bin/env bash
# The caller validates its owned cache and pinned URL before acquiring. Global
# archives are read-only inputs; only verified bytes enter the local cache.
source "${BASH_SOURCE[0]%/*}/lifecycle/cpkt-archive-cache.sh"

knurl_acquire_archive() (
  local url=$1 digest=$2 cache=$3 global_cache=$4 name=${1##*/}
  local destination temporary candidate actual
  destination="$cache/archives/sha256/$digest/$name"
  mkdir -p "${destination%/*}" "$cache/locks"
  exec 9>"$cache/locks/$digest.lock"
  flock -w 600 9
  cpkt_restore_cached_archive "$destination" "$digest"
  if [[ ! -f "$destination" ]]; then
    temporary=$(mktemp "${destination%/*}/.download.XXXXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
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
