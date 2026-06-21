#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

_x() { echo "—————————————————————————"; echo "$@"; "$@"; }
_err() { echo "$0: $@" >&2; exit 1; }

_err "no tests here yet *tumbleweed*"
