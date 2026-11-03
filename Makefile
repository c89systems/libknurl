CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR := build/native

.PHONY: help configure build prepare-kore test update-kore clean
.NOTPARALLEL:

help:
	@printf '%s\n' \
	  'make build / prepare-kore   Prepare patched Kore under build/native/kore.' \
	  'make test                  Run vendoring and incremental-build regressions.' \
	  'make update-kore KORE_REVISION=<full-sha>  Import a pristine upstream revision.' \
	  'make clean                 Remove build/, dist/ and local .cache/.' \
	  'This bootstrap does not yet build the librotmos server library.'

configure:
	"$(CMAKE)" -S . -B $(BUILD_DIR)

build: prepare-kore

prepare-kore: configure
	"$(CMAKE)" --build $(BUILD_DIR) --target kore-source --parallel 1

test: prepare-kore
	"$(CTEST)" --test-dir $(BUILD_DIR) --parallel 1 --stop-on-failure --output-on-failure --no-tests=error

update-kore:
	bash scripts/update-kore.sh '$(KORE_REVISION)'

clean:
	"$(CMAKE)" -E rm -rf -- build dist .cache
