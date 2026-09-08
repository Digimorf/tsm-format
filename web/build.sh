#!/bin/sh
# TSM - Temporal Signal Medium
# Build the reference tools into WebAssembly, for a page that plays a TSM.
#
# Copyright (c) 2026 Francesco De Simone
# SPDX-License-Identifier: Apache-2.0
#
#   ./build.sh            release
#   ./build.sh node       a build that runs under node, for testing
#
# Needs the Emscripten SDK on PATH (emcc). Everything else is in src/.
#
# Note -Dmain=tsm2wav_main: the renderer is the CLI tool exactly as it ships,
# with only its entry point renamed so tsm_web.c can call it. Nothing about the
# decoding is written twice, so the page cannot drift from bin/tsm2wav_v5.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../src"
OUT="$HERE/tsm.js"

TARGET=${1:-web}
if [ "$TARGET" = "node" ]; then
    ENVIRONMENT=node
    OUT="$HERE/tsm-node.js"
else
    ENVIRONMENT=web,worker
fi

emcc \
    -std=c99 -O2 \
    -I"$SRC" \
    "$HERE/tsm_web.c" \
    "$SRC/tsm_v5.c" \
    "$SRC/wav_io_simple.c" \
    "$SRC/indexed_common.c" \
    -Dmain=tsm2wav_main "$SRC/tsm2wav_v5.c" \
    -o "$OUT" \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=createTSM \
    -s ENVIRONMENT=$ENVIRONMENT \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_tsm_web_describe","_tsm_web_render","_tsm_web_free","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPF32","UTF8ToString","lengthBytesUTF8"]' \
    -s FILESYSTEM=1 \
    -s INVOKE_RUN=0 \
    --closure 0

echo "built $OUT"
ls -la "$OUT" "${OUT%.js}.wasm" 2>/dev/null || true
