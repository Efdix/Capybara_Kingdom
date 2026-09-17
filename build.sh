#!/bin/bash
set -e
export PATH="/opt/wonderful/bin:/usr/bin:$PATH"
export BLOCKSDS="/opt/wonderful/thirdparty/blocksds/core"
export BLOCKSDSEXT="/opt/wonderful/thirdparty/blocksds/external"
export WONDERFUL_TOOLCHAIN="/opt/wonderful"
cd /d/System/Documents/GitHub/Capybara_Kingdom
make -j4 "$@"
