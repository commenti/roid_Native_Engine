#!/usr/bin/env bash
# Vendors ByteHook headers + arm64-v8a prebuilt into libs/bytehook/
# Usage: bash scripts/vendor_bytehook.sh [BHOOK_VERSION]
# Requires: curl, unzip, sha256sum

set -euo pipefail

BHOOK_VERSION="${1:-2.0.0}"
REPO="bytedance/bhook"
RELEASE_URL="https://github.com/${REPO}/releases/download/${BHOOK_VERSION}"
ARCHIVE="bhook-${BHOOK_VERSION}-android.zip"
CHECKSUM_FILE="scripts/bytehook_checksums.sha256"

VENDOR_ROOT="$(cd "$(dirname "$0")/.." && pwd)/libs/bytehook"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "[vendor] ByteHook version : ${BHOOK_VERSION}"
echo "[vendor] Download URL     : ${RELEASE_URL}/${ARCHIVE}"
echo "[vendor] Vendor target    : ${VENDOR_ROOT}"

# ── Download ──────────────────────────────────────────────────────────────────
curl -fsSL --retry 3 \
    "${RELEASE_URL}/${ARCHIVE}" \
    -o "${TMP_DIR}/${ARCHIVE}"

# ── Checksum verification (optional but recommended) ──────────────────────────
if [[ -f "${CHECKSUM_FILE}" ]]; then
    echo "[vendor] Verifying checksum..."
    (cd "${TMP_DIR}" && sha256sum --check --ignore-missing \
        "$(cd "$(dirname "$0")/.." && pwd)/${CHECKSUM_FILE}")
else
    echo "[vendor] WARNING: No checksum file at ${CHECKSUM_FILE}. Skipping verification."
fi

# ── Unpack ────────────────────────────────────────────────────────────────────
unzip -q "${TMP_DIR}/${ARCHIVE}" -d "${TMP_DIR}/unpacked"

# ── Install headers ───────────────────────────────────────────────────────────
mkdir -p "${VENDOR_ROOT}/include"
find "${TMP_DIR}/unpacked" -name "bytehook.h" -exec \
    cp -v {} "${VENDOR_ROOT}/include/bytehook.h" \;

# ── Install prebuilt .so for each ABI present in the archive ─────────────────
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
    SO_SRC=$(find "${TMP_DIR}/unpacked" -path "*/${ABI}/libbytehook.so" | head -n1)
    if [[ -n "${SO_SRC}" ]]; then
        mkdir -p "${VENDOR_ROOT}/${ABI}"
        cp -v "${SO_SRC}" "${VENDOR_ROOT}/${ABI}/libbytehook.so"
        echo "[vendor] Installed ${ABI}/libbytehook.so"
    fi
done

# ── Sanity check ─────────────────────────────────────────────────────────────
if [[ ! -f "${VENDOR_ROOT}/include/bytehook.h" ]]; then
    echo "[vendor] FATAL: bytehook.h was not found in the archive. Check release structure."
    exit 1
fi

echo "[vendor] Done. Verify with: ls -lR ${VENDOR_ROOT}"