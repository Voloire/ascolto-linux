#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${ROOT_DIR}/third_party/whistle"
mkdir -p "${DEST_DIR}"

if [ ! -f "${DEST_DIR}/needle.h" ]; then
    echo "Fetching needle.h..."
    curl -fsSL "https://huggingface.co/Cactus-Compute/needle3/resolve/main/linux-x86_64/needle.h" \
        -o "${DEST_DIR}/needle.h"
fi

if [ ! -f "${DEST_DIR}/libneedle.a" ]; then
    echo "Fetching libneedle.a (linux-x86_64)..."
    curl -fsSL "https://huggingface.co/Cactus-Compute/needle3/resolve/main/linux-x86_64/libneedle.a" \
        -o "${DEST_DIR}/libneedle.a"
fi

if [ ! -f "${DEST_DIR}/libneedle.so" ]; then
    echo "Fetching libneedle.so (linux-x86_64 with static libc++)..."
    TMP_WHL="$(mktemp /tmp/needle-XXXXXX.whl)"
    curl -fsSL "https://huggingface.co/Cactus-Compute/needle3/resolve/main/python/cactus_needle-3.3.1-py3-none-manylinux2014_x86_64.whl" \
        -o "${TMP_WHL}"
    python3 -c "import zipfile; z=zipfile.ZipFile('${TMP_WHL}'); open('${DEST_DIR}/libneedle.so','wb').write(z.read('needle/libneedle3.so'))"
    rm -f "${TMP_WHL}"
fi

if [ ! -f "${DEST_DIR}/whistle.cact" ]; then
    echo "Fetching whistle.cact (17 MB)..."
    curl -fsSL "https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact" \
        -o "${DEST_DIR}/whistle.cact"
fi

echo "Whistle SDK artifacts ready in ${DEST_DIR}"
