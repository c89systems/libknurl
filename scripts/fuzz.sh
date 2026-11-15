#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
[[ $# == 3 && $1 == /* && $2 =~ ^(smoke|long)$ && $3 =~ ^[1-9][0-9]*$ ]] || {
  printf 'usage: fuzz.sh ABSOLUTE_BUILD_DIR smoke|long SECONDS\n' >&2; exit 1;
}
build_dir=$1 mode=$2 seconds=$3
cmake "-DKNURL_CHECK_PATH=$build_dir" -P "$root/cmake/OwnedPaths.cmake"
[[ -x "$build_dir/knurl_fuzz" ]] || { printf 'Build the instrumented parsers first\n' >&2; exit 1; }
output="$build_dir/$mode"
check_findings() {
  local stats_file=$1 field number
  for field in saved_crashes saved_hangs; do
    number=$(sed -n "s/^$field[[:space:]]*:[[:space:]]*//p" "$stats_file")
    [[ $number == 0 ]] || { printf '%s=%s; preserve and investigate %s\n' "$field" "$number" "$output" >&2; return 1; }
  done
}
# AFL can rotate old findings on resume. Never let a second invocation turn an
# unresolved failure into a fresh, empty finding count.
if [[ -f "$output/default/fuzzer_stats" ]]; then
  check_findings "$output/default/fuzzer_stats"
fi
description=$("$root/scripts/lifecycle/cpkt-aflpp.sh" discover)
afl_fuzz=$(sed -n 's/^afl_fuzz=//p' <<<"$description")
showmap=$(sed -n 's/^afl_showmap=//p' <<<"$description")
# Only the harness is non-dumpable; signal crashes remain visible to AFL.
# The host core handler, CPU policy and affinity are never modified.
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 AFL_NO_AFFINITY=1 AFL_SKIP_CPUFREQ=1 AFL_NO_UI=1
"$showmap" -q -o "$build_dir/coverage.map" -- "$build_dir/knurl_fuzz" "$root/tests/fuzz/corpus/chunks"
[[ -s "$build_dir/coverage.map" ]] || { printf 'No AFL instrumentation coverage\n' >&2; exit 1; }
if [[ -e "$output/default/fuzzer_stats" ]]; then
  seeds=-
else
  seeds="$root/tests/fuzz/corpus"
fi
"$afl_fuzz" -i "$seeds" -o "$output" -x "$root/tests/fuzz/parsers.dict" -V "$seconds" -m none -t 1000 -- "$build_dir/knurl_fuzz" @@
stats="$output/default/fuzzer_stats"
[[ -f "$stats" ]] || { printf 'AFL produced no run statistics\n' >&2; exit 1; }
check_findings "$stats"
executions=$(sed -n 's/^execs_done[[:space:]]*:[[:space:]]*//p' "$stats")
[[ $executions =~ ^[1-9][0-9]*$ ]] || { printf 'AFL executed no inputs\n' >&2; exit 1; }
printf 'AFL %s: %s cumulative executions; no saved crashes/hangs; %s\n' "$mode" "$executions" "$output"
