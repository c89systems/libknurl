#!/usr/bin/env bash
set -euo pipefail
case "$(uname -s):$(uname -m)" in
  Linux:x86_64) printf '%s\n' x86_64-linux-gnu ;;
  Linux:aarch64) printf '%s\n' aarch64-linux-gnu ;;
  Linux:armv7l) printf '%s\n' armhf-linux-gnu ;;
  Darwin:arm64) printf '%s\n' arm64-apple-darwin ;;
  *) printf '%s\n' unsupported ;;
esac
