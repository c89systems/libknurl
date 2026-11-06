#!/usr/bin/env bash
set -euo pipefail
root=$1
test_root=$2
rm -rf -- "$test_root"
mkdir -p "$test_root/upstream/src" "$test_root/consumer/scripts" \
    "$test_root/consumer/cmake" "$test_root/consumer/vendor/kore/src" \
    "$test_root/consumer/patches/kore"
upstream="$test_root/upstream"
consumer="$test_root/consumer"

# Real Git repositories exercise commit identity and snapshot exports offline.
commit_fixture() {
    git -C "$1" add .
    git -C "$1" -c user.name=fixture -c user.email=fixture@example.invalid \
        -c core.hooksPath=/dev/null -c commit.gpgsign=false commit -q -m 'test fixture'
}
git init -q --initial-branch=trunk "$upstream"
printf 'upstream\n' > "$upstream/src/kore.c"
commit_fixture "$upstream"
initial=$(git -C "$upstream" rev-parse HEAD)
printf 'compatible upgrade\n' > "$upstream/new-file"
commit_fixture "$upstream"
compatible=$(git -C "$upstream" rev-parse HEAD)
printf 'conflicting upgrade\n' > "$upstream/src/kore.c"
commit_fixture "$upstream"
conflicting=$(git -C "$upstream" rev-parse HEAD)

cp "$root/scripts/update-kore.sh" "$consumer/scripts/"
cp "$root/cmake/PrepareKore.cmake" "$root/cmake/OwnedPaths.cmake" "$consumer/cmake/"
printf 'upstream\n' > "$consumer/vendor/kore/src/kore.c"
printf '%s\n' "$initial" > "$consumer/vendor/kore.revision"
printf '%s\n' "$upstream" > "$consumer/vendor/kore.url"
printf '%s\n' change.patch > "$consumer/patches/kore/series"
cat > "$consumer/patches/kore/change.patch" <<'PATCH'
--- a/src/kore.c
+++ b/src/kore.c
@@ -1 +1 @@
-upstream
+patched
PATCH
git init -q --initial-branch=trunk "$consumer"
commit_fixture "$consumer"

upgrade() {
    bash "$consumer/scripts/update-kore.sh" "$1" > "$test_root/upgrade.log" 2>&1
}
expect_failure() {
    if upgrade "$1"; then
        echo "upgrade unexpectedly succeeded: $1 (expected $2)" >&2
        exit 1
    fi
    if ! grep -Fq "$2" "$test_root/upgrade.log"; then
        echo "upgrade did not report: $2" >&2
        cat "$test_root/upgrade.log" >&2
        exit 1
    fi
}
assert_no_scratch() {
    local residue
    shopt -s nullglob
    residue=("$consumer"/build/kore-upgrade.*)
    [[ ${#residue[@]} == 0 ]] || { echo 'upgrade left temporary state' >&2; exit 1; }
}

if ! upgrade "$compatible"; then
    cat "$test_root/upgrade.log" >&2
    exit 1
fi
[[ $(cat "$consumer/vendor/kore.revision") == "$compatible" ]]
[[ $(cat "$consumer/vendor/kore/src/kore.c") == upstream ]]
[[ $(cat "$consumer/vendor/kore/new-file") == 'compatible upgrade' ]]
[[ ! -e $consumer/vendor/kore/.git && ! -e $consumer/vendor/kore/.patched ]]
assert_no_scratch

# Uncommitted snapshot/provenance must never be overwritten by an upgrade.
expect_failure "$conflicting" 'local changes'
commit_fixture "$consumer"

# A patch conflict identifies the patch, preserves the snapshot and cleans up.
expect_failure "$conflicting" 'Kore patch failed: change.patch'
[[ $(cat "$consumer/vendor/kore.revision") == "$compatible" ]]
[[ $(cat "$consumer/vendor/kore/src/kore.c") == upstream ]]
assert_no_scratch

# Refs and abbreviated hashes are not exact revision pins.
for invalid in trunk deadbeef ''; do
    expect_failure "$invalid" 'full-upstream-commit-sha'
done
assert_no_scratch
