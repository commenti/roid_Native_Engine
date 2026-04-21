#!/usr/bin/env bash
# Vendors ByteHook by cloning the official repo and copying
# the prebuilt headers + AAR-extracted .so into libs/bytehook/
# Usage: bash scripts/vendor_bytehook.sh [TAG]
# TAG defaults to the latest stable release tag.

set -euo pipefail

BHOOK_TAG="${1:-v2.0.0}"
REPO_URL="https://github.com/bytedance/bhook.git"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${ROOT_DIR}/libs/bytehook"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "[vendor] Cloning bhook tag=${BHOOK_TAG}"
git clone --depth 1 --branch "${BHOOK_TAG}" \
    "${REPO_URL}" "${TMP_DIR}/bhook" \
    2>&1 | grep -E '(Cloning|error|fatal)' || true

CLONE_DIR="${TMP_DIR}/bhook"

if [[ ! -d "${CLONE_DIR}" ]]; then
    echo "[vendor] FATAL: clone failed — ${CLONE_DIR} not found"
    exit 1
fi

# ── Install headers ───────────────────────────────────────────────────────────
# Header lives at: bytehook/include/bytehook.h inside the repo
HEADER_SRC="${CLONE_DIR}/bytehook/include/bytehook.h"
if [[ ! -f "${HEADER_SRC}" ]]; then
    echo "[vendor] FATAL: bytehook.h not found at expected path: ${HEADER_SRC}"
    echo "[vendor] Repo tree (3 levels):"
    find "${CLONE_DIR}" -maxdepth 3 -name "*.h" || true
    exit 1
fi

mkdir -p "${VENDOR_DIR}/include"
cp -v "${HEADER_SRC}" "${VENDOR_DIR}/include/bytehook.h"

# ── Install prebuilt .so from the repo's prebuilt/ directory ─────────────────
# bhook ships prebuilts at: bytehook/src/main/cpp/libs/<ABI>/libbytehook.so
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
    SO_SRC=$(find "${CLONE_DIR}" \
        -path "*/${ABI}/libbytehook.so" | head -n1)
    if [[ -n "${SO_SRC}" ]]; then
        mkdir -p "${VENDOR_DIR}/${ABI}"
        cp -v "${SO_SRC}" "${VENDOR_DIR}/${ABI}/libbytehook.so"
        echo "[vendor] Installed ${ABI}/libbytehook.so"
    else
        echo "[vendor] WARNING: libbytehook.so not found for ABI=${ABI} — skipping."
    fi
done

# ── Final assertion ───────────────────────────────────────────────────────────
if [[ ! -f "${VENDOR_DIR}/include/bytehook.h" ]]; then
    echo "[vendor] FATAL: Header copy failed."
    exit 1
fi

echo "[vendor] Done: $(ls -lh "${VENDOR_DIR}/include/bytehook.h")"
