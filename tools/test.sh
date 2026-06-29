#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

_x() { echo "—————————————————————————"; echo "$@"; "$@"; }
_err() { echo "$0: $@" >&2; exit 1; }

for f in examples/*; do
    [[ "$f" != .* && "$f" != _* ]] || continue
    if [[ "$f" == *.c || -d "$f" ]]; then
        _x ./tools/dev.sh "$f" run=0 format=0
        _x ./tools/dev.sh "$f" run=0 format=0 native=1
    fi
done

echo "All PASS"
