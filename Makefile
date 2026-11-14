CMAKE ?= cmake
CTEST ?= ctest
CONFIG ?= debug
JOBS ?= 1
DEFAULT_TARGET := x86_64-linux-gnu
TARGET ?= $(DEFAULT_TARGET)
SUPPORTED_TARGETS := $(shell bash scripts/lifecycle/cpkt-toolchains.sh targets)
BUILD_TYPE_debug := Debug
BUILD_TYPE_release := Release
ifeq ($(BUILD_TYPE_$(CONFIG)),)
$(error CONFIG must be debug or release)
endif
ifeq ($(filter $(TARGET),$(SUPPORTED_TARGETS)),)
$(error Unsupported TARGET: $(TARGET))
endif
BUILD_DIR ?= build/$(if $(filter $(TARGET),$(DEFAULT_TARGET)),,$(TARGET)/)$(CONFIG)
TOOLCHAIN := cmake/toolchains/selected.cmake
PREFIX ?= $(CURDIR)/build/install/$(TARGET)/$(CONFIG)
CANDIDATE_VERSION ?=
VERSION_ARG := -DKNURL_CANDIDATE_VERSION=$(CANDIDATE_VERSION)
FUZZ_SECONDS ?= 300
FUZZ_BUILD_DIR := build/release/fuzz
ifneq ($(filter memcheck fuzz fuzz-build fuzz-smoke,$(MAKECMDGOALS)),)
ifneq ($(TARGET),x86_64-linux-gnu)
$(error Valgrind/AFL profiles require TARGET=x86_64-linux-gnu)
endif
endif
ifneq ($(filter qemu-smoke,$(MAKECMDGOALS)),)
ifeq ($(filter $(TARGET),aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl),)
$(error QEMU smoke requires one of the four ARM Linux TARGET values)
endif
endif

.PHONY: help deps deps-all toolchain toolchains configure build build-all sdk-package sdk-verify prepare-kore test install format format-check update-kore clean
.NOTPARALLEL:
.PHONY: memcheck qemu-smoke fuzz-toolchain fuzz-build fuzz-smoke fuzz verify-all prerelease release-check release-local

help:
	@printf '%s\n' \
	  'make build                 Compile the selected target (default: x86_64-linux-gnu).' \
	  'make build-all             Compile all seven lifecycle/cpkt targets, serially.' \
	  'make sdk-package           Stage the selected-target Core SDK binary rehearsal.' \
	  'make sdk-verify            Verify that archive and its relocated example consumers.' \
	  'make test                  Build, then run registered CTest checks.' \
	  'make memcheck              Valgrind C library checks; reuses the selected GNU build.' \
	  'make qemu-smoke            Basic C/HTTP checks for an ARM Linux TARGET; no fuzzing.' \
	  'make fuzz-toolchain        Explicitly prepare pinned native GNU AFL++ tooling.' \
	  'make fuzz-smoke / fuzz     AFL++ parser smoke (10s) / longer run (FUZZ_SECONDS=300).' \
	  'make verify-all            Build each target once, then run native, memory, QEMU and SDK gates.' \
	  'make prerelease            Run verify-all CONFIG=release without publishing.' \
	  'make release-check         Read-only signed/default-branch/lightweight-tag check.' \
	  'make release-local         Check release identity, clean once, then run the Release gate.' \
	  'make install               Install to PREFIX (default: owned build/install/...). ' \
	  'make prepare-kore          Prepare patched sources without dependency SDKs.' \
	  'make deps                  Acquire checksum-pinned released SDKs.' \
	  'make deps-all              Prepare SDK prefixes for all seven targets.' \
	  'make toolchain             Explicitly provision the selected pinned compiler.' \
	  'make toolchains            Explicitly provision all supported toolchains.' \
	  'make format / format-check Format or check project C/C++ sources.' \
	  'make update-kore KORE_REVISION=<full-sha>  Import a pristine upstream revision.' \
	  'make clean                 Remove build/, dist/ and local .cache/.' \
	  'Options: TARGET=<SDK target> CONFIG=debug|release JOBS=1 PREFIX=<owned path>.' \
	  'Default selected target builds in build/debug or build/release; other targets use build/<target>/<config>.'

deps:
	bash scripts/deps.sh '$(TARGET)'

toolchain:
	bash scripts/lifecycle/cpkt-toolchains.sh ensure '$(TARGET)'

toolchains:
	bash scripts/lifecycle/cpkt-toolchains.sh ensure all

deps-all:
	@for target in $(SUPPORTED_TARGETS); do $(MAKE) --no-print-directory deps TARGET="$$target" || exit $$?; done

build-all:
	@for target in $(SUPPORTED_TARGETS); do $(MAKE) --no-print-directory build TARGET="$$target" || exit $$?; done

configure: deps
	"$(CMAKE)" -S . -B '$(BUILD_DIR)' -G Ninja -DCMAKE_BUILD_TYPE='$(BUILD_TYPE_$(CONFIG))' -DBUILD_TESTING=ON -DCMAKE_TOOLCHAIN_FILE='$(TOOLCHAIN)' -DKNURL_TARGET='$(TARGET)' '$(VERSION_ARG)'

build: configure
	"$(CMAKE)" --build '$(BUILD_DIR)' --parallel '$(JOBS)'

memcheck: build
	"$(CMAKE)" -S cmake/checks -B '$(BUILD_DIR)/checks/memory' '-DKNURL_BUILD=$(CURDIR)/$(BUILD_DIR)' -DCHECK_KIND=memory
	"$(CTEST)" --test-dir '$(BUILD_DIR)/checks/memory' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error

qemu-smoke: build
	"$(CMAKE)" -S cmake/checks -B '$(BUILD_DIR)/checks/qemu' '-DKNURL_BUILD=$(CURDIR)/$(BUILD_DIR)' -DCHECK_KIND=qemu
	"$(CTEST)" --test-dir '$(BUILD_DIR)/checks/qemu' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error

fuzz-toolchain:
	bash scripts/lifecycle/cpkt-aflpp.sh ensure

fuzz-build:
	$(MAKE) --no-print-directory build TARGET=x86_64-linux-gnu CONFIG=release
	"$(CMAKE)" --install '$(CURDIR)/build/release' --prefix '$(CURDIR)/build/release/core-sdk'
	"$(CMAKE)" -S tests/fuzz -B '$(FUZZ_BUILD_DIR)' -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo '-DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/$(TOOLCHAIN)' -DKNURL_TARGET=x86_64-linux-gnu -DKNURL_AFL=ON '-DKNURL_CORE_PREFIX=$(CURDIR)/build/release/core-sdk' '-DKNURL_SDK_ROOT=$(CURDIR)/.cache/sdk/x86_64-linux-gnu' '$(VERSION_ARG)'
	"$(CMAKE)" --build '$(FUZZ_BUILD_DIR)' --parallel '$(JOBS)'

fuzz-smoke: fuzz-build
	"$(CTEST)" --test-dir '$(FUZZ_BUILD_DIR)' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error
	bash scripts/fuzz.sh '$(CURDIR)/$(FUZZ_BUILD_DIR)' smoke 10

fuzz: fuzz-build
	bash scripts/fuzz.sh '$(CURDIR)/$(FUZZ_BUILD_DIR)' long '$(FUZZ_SECONDS)'

verify-all:
	bash scripts/verify-all.sh '$(CONFIG)' '$(JOBS)' '$(CANDIDATE_VERSION)'

prerelease:
	$(MAKE) --no-print-directory verify-all CONFIG=release

release-check:
	"$(CMAKE)" '$(VERSION_ARG)' -P cmake/ReleaseCheck.cmake

release-local: release-check
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory verify-all CONFIG=release CANDIDATE_VERSION=
	"$(CMAKE)" -P cmake/ReleaseArtifacts.cmake

sdk-package: build
	"$(CMAKE)" '-DBUILD_DIR=$(CURDIR)/$(BUILD_DIR)' -P cmake/Package.cmake

sdk-verify:
	"$(CMAKE)" '-DBUILD_DIR=$(CURDIR)/$(BUILD_DIR)' -P cmake/VerifySDK.cmake

prepare-kore:
	"$(CMAKE)" -S cmake/source -B build/source -DKNURL_ROOT='$(CURDIR)'
	"$(CMAKE)" --build build/source --target kore-source --parallel '$(JOBS)'

test: build
	"$(CTEST)" --test-dir '$(BUILD_DIR)' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error

install: build
	"$(CMAKE)" '-DKNURL_CHECK_PATH=$(PREFIX)' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" --install '$(BUILD_DIR)' --prefix '$(PREFIX)'

format:
	clang-format -i include/knurl/*.h src/*.c src/*.h examples/*.c tests/*.c tests/*.h tests/*.cpp tests/fuzz/*.c

format-check:
	clang-format --dry-run --Werror include/knurl/*.h src/*.c src/*.h examples/*.c tests/*.c tests/*.h tests/*.cpp tests/fuzz/*.c

update-kore:
	bash scripts/update-kore.sh '$(KORE_REVISION)'

clean:
	"$(CMAKE)" '-DKNURL_CHECK_PATH=$(CURDIR)/build' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" '-DKNURL_CHECK_PATH=$(CURDIR)/dist' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" '-DKNURL_CHECK_PATH=$(CURDIR)/.cache' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" -E rm -rf -- build dist .cache
