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

.PHONY: help deps deps-all toolchain toolchains configure build build-all prepare-kore test install format format-check update-kore clean
.NOTPARALLEL:

help:
	@printf '%s\n' \
	  'make build                 Compile the selected target (default: x86_64-linux-gnu).' \
	  'make build-all             Compile all seven lifecycle/cpkt targets, serially.' \
	  'make test                  Build, then run registered CTest checks.' \
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
	"$(CMAKE)" -S . -B '$(BUILD_DIR)' -G Ninja -DCMAKE_BUILD_TYPE='$(BUILD_TYPE_$(CONFIG))' -DCMAKE_TOOLCHAIN_FILE='$(TOOLCHAIN)' -DROTMOS_TARGET='$(TARGET)'

build: configure
	"$(CMAKE)" --build '$(BUILD_DIR)' --parallel '$(JOBS)'

prepare-kore:
	"$(CMAKE)" -S cmake/source -B build/source -DROTMOS_ROOT='$(CURDIR)'
	"$(CMAKE)" --build build/source --target kore-source --parallel '$(JOBS)'

test: build
	"$(CTEST)" --test-dir '$(BUILD_DIR)' --parallel 1 --stop-on-failure --output-on-failure --no-tests=error

install: build
	"$(CMAKE)" '-DROTMOS_CHECK_PATH=$(PREFIX)' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" --install '$(BUILD_DIR)' --prefix '$(PREFIX)'

format:
	clang-format -i include/rotmos/*.h src/*.c src/*.h tests/*.c tests/*.cpp

format-check:
	clang-format --dry-run --Werror include/rotmos/*.h src/*.c src/*.h tests/*.c tests/*.cpp

update-kore:
	bash scripts/update-kore.sh '$(KORE_REVISION)'

clean:
	"$(CMAKE)" '-DROTMOS_CHECK_PATH=$(CURDIR)/build' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" '-DROTMOS_CHECK_PATH=$(CURDIR)/dist' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" '-DROTMOS_CHECK_PATH=$(CURDIR)/.cache' -P cmake/OwnedPaths.cmake
	"$(CMAKE)" -E rm -rf -- build dist .cache
