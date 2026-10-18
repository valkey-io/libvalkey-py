#!/usr/bin/env bash
# Run the tests with native memory checking in a disposable Linux container.
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

exec docker run --rm \
    --mount "type=bind,src=$repo,dst=/source" \
    ghcr.io/nascheme/cpython-asan:3.14 \
    bash /source/scripts/run-memory-tests.sh "$@"
