#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$repo"

# Only the test process is checked for shutdown leaks. Build/install helpers
# still fail on invalid accesses, but not retained allocations.
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:exitcode=1"
python -m pip install setuptools wheel pytest

# setup.py compiles both our extension and bundled libvalkey sources.
export CFLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined"
export LDFLAGS="-fsanitize=address,undefined"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
python setup.py build_ext --force --inplace

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:exitcode=1:symbolize=1"
python -m pytest -s -v tests/ "$@"
