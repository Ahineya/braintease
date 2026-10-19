#!/usr/bin/env bash
# Minimal Riplix shell on the Brainfuck CPU, with a fresh writable disk.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/ports/fuzix/rbf/run.py" "$@"
