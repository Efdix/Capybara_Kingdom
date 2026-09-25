#!/bin/bash
set -e
export PATH="/opt/wonderful/bin:/usr/bin:$PATH"
export PYTHONIOENCODING=utf-8
export BLOCKSDS="/opt/wonderful/thirdparty/blocksds/core"
export BLOCKSDSEXT="/opt/wonderful/thirdparty/blocksds/external"
export WONDERFUL_TOOLCHAIN="/opt/wonderful"

# The compiler writes its scratch files to TMP/TEMP and silently falls back to
# C:\WINDOWS\ when they are empty, which it cannot write to, and then exits
# with "Error 127". That happens when this script is reached from a shell that
# does not provide a Windows path (Git Bash hands over /tmp, which MSYS2 drops).
# MSYS2's own /tmp is a real Windows directory, so use it as the fallback.
if [ -z "$TMP" ] || [ -z "$TEMP" ]; then
    win_tmp="$(cygpath -m /tmp)"
    export TMP="${TMP:-$win_tmp}"
    export TEMP="${TEMP:-$win_tmp}"
fi

cd /d/System/Documents/GitHub/Capybara_Kingdom

# The self-driving development ROMs are asked for as --autoplay, --menu, --tap,
# --boss and --ending. They are make targets rather than make options, so the
# leading dashes have to go before make sees them.
targets=()
for arg in "$@"; do
    case "$arg" in
        --*) targets+=("${arg#--}") ;;
        *)   targets+=("$arg") ;;
    esac
done

make -j4 "${targets[@]}"
