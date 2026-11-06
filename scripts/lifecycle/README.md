# Lifecycle helpers

These Bash helpers are copied from the
`pkt-systems-cmake-lifecycle` skill's `scripts/` directory. They provide host Bash
validation, verified archive reuse, the pinned compiler collection resolver and
the matching AFL++ GCC-plugin/runtime resolver. The AFL helper's sibling-resolver
path is adjusted for this directory layout; tooling pins are unchanged.
Keep updates synchronized with that skill; project dependency pins and build
sequencing live in `../dependency-pins.tsv`, `../deps.sh`, Make and CMake.

Compiler discovery is read-only. Provisioning is an explicit `make toolchain`
operation and preserves the shared cpkt workstation cache across `make clean`.
