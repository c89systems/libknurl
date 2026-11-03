#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 || ! $1 =~ ^[0-9a-fA-F]{40}$ ]]; then
    echo 'usage: bash scripts/update-kore.sh <full-upstream-commit-sha>' >&2
    exit 2
fi
revision=${1,,}
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
IFS= read -r upstream_url < "$repo_root/vendor/kore.url"
if [[ -z $upstream_url ]]; then
    echo 'vendor/kore.url must identify the upstream Git repository' >&2
    exit 2
fi
for tool in git cmake tar mktemp; do
    command -v "$tool" >/dev/null || { echo "required tool missing: $tool" >&2; exit 2; }
done
snapshot_status=$(git -C "$repo_root" status --porcelain --untracked-files=all -- \
    vendor/kore vendor/kore.revision vendor/kore.url)
if [[ -n $snapshot_status ]]; then
    echo 'Kore snapshot or provenance has local changes; commit or restore them before upgrading' >&2
    exit 2
fi
if [[ -L $repo_root/build ]]; then
    echo 'build/ must be an owned directory, not a symlink' >&2
    exit 2
fi
mkdir -p "$repo_root/build"
work=$(mktemp -d "$repo_root/build/kore-upgrade.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

git clone --quiet --no-checkout -- "$upstream_url" "$work/upstream"
if ! git -C "$work/upstream" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$work/upstream" fetch --quiet origin "$revision"
fi
actual=$(git -C "$work/upstream" rev-parse --verify "$revision^{commit}")
if [[ $actual != "$revision" ]]; then
    echo "upstream commit identity mismatch: expected $revision, got $actual" >&2
    exit 1
fi
mkdir "$work/source"
git -C "$work/upstream" archive "$revision" | tar -x -C "$work/source"

# A conflicting upgrade leaves the tracked snapshot and revision untouched.
cmake "-DKORE_SOURCE=$work/source" "-DKORE_PATCHES=$repo_root/patches/kore" \
    "-DKORE_OUTPUT=$work/patched" -P "$repo_root/cmake/PrepareKore.cmake"

rm -rf -- "$repo_root/vendor/kore"
mv -- "$work/source" "$repo_root/vendor/kore"
printf '%s\n' "$revision" > "$repo_root/vendor/kore.revision"
printf 'Imported pristine Kore %s. Run make test and inspect the diff before committing.\n' "$revision"
