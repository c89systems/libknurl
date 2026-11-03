CMAKE ?= cmake
CTEST ?= ctest
CONFIG ?= Debug
JOBS ?= 1
NATIVE_TARGET := $(shell bash scripts/native-target.sh)
TARGET ?= $(NATIVE_TARGET)
BUILD_DIR ?= build/$(if $(filter $(TARGET),$(NATIVE_TARGET)),native,$(TARGET))/$(CONFIG)
TOOLCHAIN ?= cmake/toolchains/bootlin.cmake
PREFIX ?= $(CURDIR)/build/install/$(TARGET)/$(CONFIG)

.PHONY: help deps toolchain configure build prepare-kore test install format format-check update-kore clean
.NOTPARALLEL:

help:
	@printf '%s\n' \
	  'make build                 Compile the C89 core and private Kore objects.' \
	  'make test                  Build, then run registered CTest checks.' \
	  'make install               Install to PREFIX (default: owned build/install/...). ' \
	  'make prepare-kore          Prepare patched sources without dependency SDKs.' \
	  'make deps                  Acquire checksum-pinned released SDKs.' \
	  'make toolchain             Explicitly provision the selected pinned compiler.' \
	  'make format / format-check Format or check project C/C++ sources.' \
	  'make update-kore KORE_REVISION=<full-sha>  Import a pristine upstream revision.' \
	  'make clean                 Remove build/, dist/ and local .cache/.' \
	  'Options: TARGET=<SDK target> CONFIG=Debug|Release JOBS=1 PREFIX=<owned path>.'

deps:
	bash scripts/deps.sh '$(TARGET)'

toolchain:
	bash scripts/lifecycle/cpkt-toolchains.sh ensure '$(TARGET)'

configure: deps
	"$(CMAKE)" -S . -B '$(BUILD_DIR)' -G Ninja -DCMAKE_BUILD_TYPE='$(CONFIG)' -DCMAKE_TOOLCHAIN_FILE='$(TOOLCHAIN)' -DROTMOS_TARGET='$(TARGET)'

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
