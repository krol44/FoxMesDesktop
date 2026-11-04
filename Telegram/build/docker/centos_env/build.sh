#!/bin/bash
set -e

cd Telegram
if [ "${FOXMES_DISTRIBUTED_PHASE:-compile}" = prepare ] || [ "${FOXMES_DISTRIBUTED_PHASE:-compile}" = shard ] || [ "${FOXMES_DISTRIBUTED_PHASE:-compile}" = link ]; then
    export CC=gcc
    export CXX=g++
    if [ "${FOXMES_COLD_BUILD:-0}" = 1 ]; then
        export CCACHE_DISABLE=1
    fi
    ./configure.sh -G "Ninja Multi-Config" \
        -D CMAKE_PROJECT_INCLUDE=/usr/src/tdesktop/Telegram/build/foxmes/distributed.cmake \
        -D "FOXMES_DISTRIBUTED_PYTHON=$(command -v python3)" "$@"
    arguments=(
        build/foxmes/distributed-build.py "$FOXMES_DISTRIBUTED_PHASE"
        --build-dir /usr/src/tdesktop/out
        --revision "$FOXMES_BUILD_REVISION"
        --count 4 --jobs "${FOXMES_BUILD_JOBS:-2}"
    )
    if [ "$FOXMES_DISTRIBUTED_PHASE" = prepare ]; then
        arguments+=(--archive /usr/src/tdesktop/artifacts/transfer/linux-objects-prepare.tar.gz)
    elif [ "$FOXMES_DISTRIBUTED_PHASE" = shard ]; then
        arguments+=(--index "$FOXMES_SHARD_INDEX" --archive "/usr/src/tdesktop/artifacts/transfer/linux-objects-$FOXMES_SHARD_INDEX.tar.gz" --prepared-dir /var/foxmes-prepared)
    else
        arguments+=(--objects-dir /var/foxmes-objects)
    fi
    python3 "${arguments[@]}"
    exit
fi
./configure.sh "$@"
# KEEP_GOING=1 lets ninja report every failing translation unit in one
# run instead of stopping at the first; the exit status still fails.
cmake --build ../out --config "${CONFIG:-Release}" ${KEEP_GOING:+-- -k 0}
