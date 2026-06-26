#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

_x() { echo "$@"; "$@"; }
_err() { echo "$0: $@" >&2; exit 1; }

game=${1:-}; [ $# -eq 0 ] || shift
release=0
native=0
mtl_debug=0
pes_debug=0
run=1
pb_app=
pb=

# cli arg set shell script variables: name[=value]
[ "$*" = "--help" -o -z "$game" ] && { echo "Usage: $0 <game> [var[=value] ...]"; exit 0; };
for a in "$@"; do
    case "$a" in
    *=*) k=${a%%=*}; v=${a#*=}
         declare -p "$k" >/dev/null 2>&1 || _err "unknown option '$k'"
         declare "$k=$v" ;;
    *)   k=$a
         declare -p "$k" >/dev/null 2>&1 || _err "unknown option '$k'"
         declare "$k=1" ;;
    esac
done

debug=1; [ $release = 0 ] || debug=0
asan=${asan:-$debug} # enable by default in debug builds
tests=${tests:-$debug} # enable by default in debug builds
mode=debug; [ $debug = 0 ] && mode=release

if [ -z "$pb_app" ]; then
    pb_app=$HOME/playbit/engine/_build/macos-aarch64-debug/Playbit.app
    [ -x "$pb_app/Contents/MacOS/Playbit" ] || pb_app=/Applications/Playbit.app
fi

if [ -z "$pb" ]; then
    pb=$HOME/playbit/engine/pb
    [ -x "$pb" ] || pb=$pb_app/Contents/SharedSupport/bin/pb
fi

if [ -d .git ] && command -v clang-format >/dev/null; then
    # ignore files which have "linguist-generated" or "linguist-vendored" in .gitattributes
    clang-format --Werror --style=file:tools/clang-format.yaml \
        -i $(ls *.c *.h |
             git check-attr --stdin linguist-generated linguist-vendored |
             grep -F ': unspecified' | cut -d: -f1 | sort -u)
fi

if command -v clang-format >/dev/null && ! [ -f .clangd -a .clangd -nt tools/clangd.yaml ]; then
    sed -e "s@\${REPO}@$PWD@g" -e "s@\${cc}@$pb cc@g" tools/clangd.yaml > .clangd
fi

PB_ARGS=( build )
[ $debug = 1 ] && PB_ARGS+=( -j1 --debug )
[ $pes_debug = 1 ] && PB_ARGS+=( -Xc,-DPES_DEBUG=1 )
[ -f .clangd ] && PB_ARGS+=( --compdb=o/compile_commands.json )

if [ $mtl_debug = 1 ]; then
    # export MTL_DEBUG_LAYER=1
    # export MTL_SHADER_VALIDATION=1
    export MTL_HUD_ENABLED=1
    export MTL_HUD_SCALE=0.25
    export MTL_HUD_ALIGNMENT=12
    export MTL_HUD_ENCODER_TIMING_ENABLED=1
    export MTL_HUD_ELEMENTS=memory,device,layersize,layerscale,fps,frameinterval,gputime,thermal,frameintervalgraph,presentdelay,frameintervalhistogram,metalcpu,gputimeline,shaders,framenumber,disk,fpsgraph,toplabeledcommandbuffers,toplabeledencoders
fi

game_name=$(basename $game .c)

if [ $native = 1 ]; then
    _x $pb "${PB_ARGS[@]}" --target macos -o o/$game_name-macos $game
    [ $run = 0 ] || _x exec o/$game_name-macos
else
    _x $pb "${PB_ARGS[@]}" -Xc,-msimd128 -o o/$game_name.wasm $game
    [ $run = 0 ] || _x exec "$pb_app/Contents/MacOS/Playbit" o/$game_name.wasm
fi
