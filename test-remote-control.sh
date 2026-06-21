#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

_x() { echo "$@"; "$@"; }
_err() { echo "$0: $@" >&2; exit 1; }

[ $# -gt 0 ] || {
    cat << END
Usage:

{
    printf '%s\n' '{"command":"wait","ms":250}'
    printf '%s\n' '{"command":"key_down","key":"Right","deviceKey":"Right"}'
    printf '%s\n' '{"command":"wait","ms":250}'
    printf '%s\n' '{"command":"screenshot","id":"abc123","format":"png"}'
} | $0 <game>"

END
}

game=$1
game_name=$(basename $game .c)

if [ -z "${pb_app:-}" ]; then
    pb_app=$HOME/playbit/engine/_build/macos-aarch64-debug/Playbit.app
    [ -x "$pb_app/Contents/MacOS/Playbit" ] || pb_app=/Applications/Playbit.app
fi

if [ -z "${pb:-}" ]; then
    pb=$HOME/playbit/engine/pb
    [ -x "$pb" ] || pb=$pb_app/Contents/SharedSupport/bin/pb
fi

PB_ARGS=( build -j1 --debug -Xc,-DPES_DEBUG=1 )
[ -f .clangd ] && PB_ARGS+=( --compdb=o/compile_commands.json )

# save the script's stdin
exec 3<&0

_x $pb "${PB_ARGS[@]}" -Xc,-msimd128 -o o/$game_name.wasm $game < /dev/null

"$pb_app/Contents/MacOS/Playbit" --remote-control o/$game_name.wasm <&3

# restore stdin (not strictly necessary since script ends here)
exec 3<&-
