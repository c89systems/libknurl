# Testing

Run build and test operations serially in one checkout. `make test` builds and
runs Core checks for the selected `TARGET` and `CONFIG`; Lua is a separate product.

```sh
make test
make lua
ctest --test-dir build/debug/lua --parallel 1 \
  --stop-on-failure --output-on-failure --no-tests=error
make memcheck
make qemu-smoke TARGET=armhf-linux-musl
make prerelease
```

`make prerelease` exercises the Release matrix. Its artifact inventory and the
signed release boundary are described in [Release preparation](releasing.md).
Use [Verification](verification.md) for dated results rather than inferring
coverage from a build or package filename.

## Verification profiles and reuse

The full local matrix requires x86_64 Linux, approved osxcross, Valgrind,
`qemu-arm`, `qemu-aarch64`, and the skill-pinned AFL++ GCC-plugin collection.
Provision missing AFL++ explicitly with `make fuzz-toolchain`; normal discovery
never downloads or substitutes host compilers. Git release-contract fixtures
also require `ssh-keygen`; their temporary keys and refs stay under `build/`.

| Profile | Scope |
| --- | --- |
| Native plain | Full GNU/musl Core and Lua suites, installed consumers and source reconstruction. Release test assertions remain enabled. |
| Native memory | Selected Core codec, source, storage, WebDAV, proxy and traversal cases, plus installed Lua foundation/REST/typed/XML cases in both loading modes, through Valgrind. Invalid memory/uninitialized uses and definite/indirect leaks fail. Logs are per process; no blanket suppressions. |
| ARM QEMU | Core HTTP/count/MIME smoke, XML and Kore JSON worker-stack regressions, and Lua preload/dynamic loading with basic JSON and inert routes. No full runtime suite, Valgrind or fuzzing. |
| Native fuzz | Owned proxy HTTP/chunk framing, WebSocket handshake/frame, route/range/ETag parsers. Instrumented C89 sources reuse the installed GNU Release SDK for support code. Seed replay, coverage-map presence, nonzero executions and zero saved crashes/hangs are required. |

`make verify-all` builds each selected configuration once through `lua-all`,
then checks its existing binaries. Host-tool checks run once. Native SDK tests
preserve and verify the final archive separately from their corruption fixtures;
cross SDKs get their own final-byte and relocated-consumer checks. No check invokes
another full matrix or cleans matching Core/Lua products. The default Debug/editor build
and the optimized Release build are distinct inputs, not interchangeable proof.

Installed reference consumers use a CTest SDK preparation fixture and separate
static/shared build and execution cases, retaining consumer objects and unchanged SDK
timestamps between invocations. The original install prefix is unavailable while
they configure, link and execute against the relocated SDK. Each executed program
has its own bounded deadline and diagnostic. SDK preparation, each variant's
configure/build and each execution sweep have independent 60-second limits;
selecting one variant schedules only its build and the common SDK preparation.
Build logs are `tests/consumer/{static,shared}-build.log` under the selected build
directory. JSON cases share this producer.

Client-starting `on_parent_ready` callbacks must explicitly open serving with
`knurl_server_set_serving(server, 1, error)` before launching their clients.
The callback enters `READY_PAUSED`; automatic serving begins after it returns.
The JSON runtime fixture uses a bounded pipe handshake to require its first
application response before callback return. This makes a missing serving
transition fail deterministically instead of depending on process scheduling.

There is one instrumented producer at `build/release/fuzz/`, including its clangd
database, shared by smoke and longer campaigns. The smoke budget is 10 seconds;
`make fuzz` defaults to 300 seconds. AFL's own queue resumes within the selected
campaign directory. Saved findings fail the command and remain available for
reproduction. The harness is non-dumpable; no host core handler, CPU policy or
affinity is changed. Fuzz and memory tools never run through QEMU or on macOS.
Resume refuses unresolved saved crashes/hangs before AFL can rotate their counts.

Some user-mode QEMU builds return `ENOSYS` for `get_robust_list`; that prevents
preparation of the required robust shared autoblock lock. Those emulated runs
can prove log transport but cannot supply autoblock runtime evidence. Use a
Linux execution environment with robust-list support for that check; the
library retains its worker-death recovery requirement.

## Hosted macOS diagnostics

[The macOS workflow](../.github/workflows/macos.yml) runs on branch pushes and
manual dispatch. It uses the standard ARM64 `macos-15` runner and serial Debug
and Release jobs. Each job builds Core and Lua with the selected Apple toolchain
and runs their complete native CTest suites, including SDK privacy/relocation
and installed-consumer checks. Homebrew supplies host tools, including Bash 4.4+
and GNU archive utilities; target dependencies use the ordinary checksum-pinned
SDK acquisition. The workflow records the tested commit in its toolchain step.

These runs provide native macOS diagnostics. Local osxcross verification remains
the Darwin release-artifact gate; hosted runs do not publish release products.

## Debug test thread stacks

GNU Linux Debug test executables set the process's default pthread stack size to
128 KiB before `main`. This includes the Lua test hosts, installed-consumer
fixtures and SDK example applications launched by the tests. The setting belongs
to these test applications; Core/Lua libraries and exported SDK targets do not
install it. Main-thread stack limits and build-tool worker defaults are unchanged.
Explicitly requested pthread stack sizes remain effective, and pthread-owned
stacks retain nonzero guard regions.

CTest disables glibc's pthread stack cache at process startup by appending
`glibc.pthread.stack_cache_size=0` to `GLIBC_TUNABLES`, preserving unrelated
tunables and passing the setting to child applications. This prevents reuse of
a larger cached stack from masking the 128 KiB constraint. To run a Debug test
executable directly, supply the same startup environment, for example:

```sh
GLIBC_TUNABLES="${GLIBC_TUNABLES:+${GLIBC_TUNABLES}:}glibc.pthread.stack_cache_size=0" \
  build/debug/knurl_xml_stack
```

An instrumented test application rejects startup without that cache setting.
The policy tests check actual default/nested worker sizes, a larger explicit
stack, guard faults and child-process behavior. Native musl checks independently
exercise musl's natural worker defaults; Release tests keep their normal defaults.

Owned C sources compile with `-Werror -Wvla -Walloca`. Use the project's heap
allocator for dynamic or large temporary storage, with explicit allocation-failure
handling and cleanup. Small fixed chunk buffers still need to fit the worker's
stack budget. A compiler-policy check accepts a bounded fixed buffer and requires
VLA and `alloca` fixtures to fail compilation.

## Traversal performance checks

The normal `knurl.xml-stack` and `knurl.kore-json-stack` checks assert
allocation-cost bounds as well as worker-stack correctness. The
`knurl.traversal-performance-contract` check proves that the timing comparator
accepts a 4% increase and rejects a 6% increase against its 5% budget. These
deterministic checks run without timing the machine.

For a measured before/after comparison, build `knurl_traversal_benchmark` in
Release and save its executable under owned `build/` before changing production
code. It links Core statically. Keep the selected compiler, dependency SDKs and
benchmark source/configuration identical for the candidate, and record the
baseline revision. Build the candidate Release executable, then run:

```sh
cmake -DBASELINE="$PWD/build/traversal-performance/baseline-benchmark" \
  -DCANDIDATE="$PWD/build/release/knurl_traversal_benchmark" \
  -DOUTPUT_DIR="$PWD/build/traversal-performance/results" \
  -P tests/TraversalPerformance.cmake
```

The script alternates three baseline/candidate runs. Each reports the median
of five process-CPU samples of at least 50 ms per workload; the comparator
asserts that the median of the three reported values does not regress by more
than 5%. XML covers a scalar record and depths 3/128; Kore covers narrow/wide
serialization, nested serialization and wide/nested destruction. Destruction
timing excludes tree construction. Logs retain both raw sets and the ratios.
These focused checks support traversal changes; server throughput and other
hardware/dependency configurations need their own measurements.
