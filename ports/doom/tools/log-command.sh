#!/usr/bin/env bash
set -euo pipefail
log="${1:?log path required}"
shift
mkdir -p "$(dirname "$log")"
if "$@" >"$log" 2>&1; then
  exit 0
else
  status=$?
  tail -c 16000 "$log" >&2
  printf '\nCommand failed (%s); full log: %s\n' "$status" "$log" >&2
  exit "$status"
fi
