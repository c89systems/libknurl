# Lifecycle helpers

These Bash helpers are copied without changes from the
`pkt-systems-cmake-lifecycle` skill's `scripts/` directory. They provide host Bash
validation, verified archive reuse and the pinned compiler collection resolver.
Keep updates synchronized with that skill; project dependency pins and build
sequencing live in `../dependency-pins.tsv`, `../deps.sh`, Make and CMake.

Compiler discovery is read-only. Provisioning is an explicit `make toolchain`
operation and preserves the shared cpkt workstation cache across `make clean`.
