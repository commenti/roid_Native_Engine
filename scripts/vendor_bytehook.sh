#!/usr/bin/env bash
# Vendors ByteHook by pulling the official AAR from Maven Central.
# The AAR contains the real prebuilt libbytehook.so for all ABIs
# plus the public header — no Gradle, no Prefab runtime required.
#
# Usage:
#   bash scripts/vendor_bytehook.sh [VERSION]
#   VERSION defaults to 2.0.0

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────
BHOOK_VERSION="${1:-2.0.0}"
GROUP_PATH="com/bytedance/android"
ARTIFACT="bytehook"
MAVEN_BASE="https://repo1.maven.org/maven2"
AAR_URL="${MAVEN_BASE}/${GROUP_PATH}/${ARTIFACT}/${BHOOK_VERSION}/${ARTIFACT}-${BHOOK_VERSION}.aar"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${ROOT_DIR}/libs/bytehook"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

AAR_FILE="${TMP_DIR}/${ARTIFACT}-${BHOOK_VERSION}.aar"

# ── Resolve latest version from Maven if not pinned ──────────────────────────
if [[ "${BHOOK_VERSION}" == "latest" ]]; then
    META_URL="${MAVEN_BASE}/${GROUP_PATH}/${ARTIFACT}/maven-metadata.xml"
    echo "[vendor] Resolving latest version from Maven metadata..."
    RAW="$(curl -fsSL --retry 3 "${META_URL}")"
    BHOOK_VERSION="$(echo "${RAW}" | grep -oP '(?<=<release>)[^<]+' | head -1)"
    if [[ -z "${BHOOK_VERSION}" ]]; then
        echo "[vendor] FATAL: Could not parse latest version from Maven metadata."
        echo "[vendor] Metadata response:"
        echo "${RAW}"
        exit 1
    fi
    echo "[vendor] Resolved latest version: ${BHOOK_VERSION}"
    AAR_URL="${MAVEN_BASE}/${GROUP_PATH}/${ARTIFACT}/${BHOOK_VERSION}/${ARTIFACT}-${BHOOK_VERSION}.aar"
fi

echo "[vendor] ByteHook version : ${BHOOK_VERSION}"
echo "[vendor] AAR URL          : ${AAR_URL}"
echo "[vendor] Vendor target    : ${VENDOR_DIR}"

# ── Download AAR ──────────────────────────────────────────────────────────────
echo "[vendor] Downloading AAR..."
HTTP_CODE="$(curl -fsSL --retry 3 --retry-delay 2 \
    -w "%{http_code}" \
    -o "${AAR_FILE}" \
    "${AAR_URL}")"

if [[ "${HTTP_CODE}" != "200" ]]; then
    echo "[vendor] FATAL: HTTP ${HTTP_CODE} downloading ${AAR_URL}"
    echo "[vendor] Verify the version exists at:"
    echo "[vendor]   ${MAVEN_BASE}/${GROUP_PATH}/${ARTIFACT}/"
    exit 1
fi

if [[ ! -s "${AAR_FILE}" ]]; then
    echo "[vendor] FATAL: Downloaded AAR is empty."
    exit 1
fi

echo "[vendor] Downloaded: $(du -sh "${AAR_FILE}" | cut -f1) bytes"

# ── Extract AAR (it is a ZIP) ─────────────────────────────────────────────────
EXTRACT_DIR="${TMP_DIR}/aar_contents"
mkdir -p "${EXTRACT_DIR}"
unzip -q "${AAR_FILE}" -d "${EXTRACT_DIR}"

echo "[vendor] AAR contents:"
find "${EXTRACT_DIR}" -maxdepth 3 | sed 's#^#  #'

# ── Extract header from jni/ or prefab/ tree ──────────────────────────────────
# AAR Prefab layout: prefab/modules/bytehook/include/bytehook.h
# Fallback: any bytehook.h anywhere in the AAR
HEADER_SRC="$(find "${EXTRACT_DIR}" -type f -name "bytehook.h" | head -1 || true)"
if [[ -z "${HEADER_SRC}" ]]; then
    echo "[vendor] FATAL: bytehook.h not found inside AAR."
    echo "[vendor] All headers in AAR:"
    find "${EXTRACT_DIR}" -name "*.h" | sed 's#^#  #' || true
    exit 1
fi

mkdir -p "${VENDOR_DIR}/include"
cp -f "${HEADER_SRC}" "${VENDOR_DIR}/include/bytehook.h"
echo "[vendor] Installed header: ${VENDOR_DIR}/include/bytehook.h"

# ── Extract prebuilt .so for each ABI ─────────────────────────────────────────
# AAR Prefab layout: prefab/modules/bytehook/libs/android.<ABI>/libbytehook.so
# AAR JNI layout  : jni/<ABI>/libbytehook.so
INSTALLED_COUNT=0
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
    SO_SRC="$(find "${EXTRACT_DIR}" -type f \
        \( -path "*/${ABI}/libbytehook.so" \
        -o -path "*/android.${ABI}/libbytehook.so" \) \
        | head -1 || true)"

    if [[ -n "${SO_SRC}" ]]; then
        mkdir -p "${VENDOR_DIR}/${ABI}"
        cp -f "${SO_SRC}" "${VENDOR_DIR}/${ABI}/libbytehook.so"
        echo "[vendor] Installed ${ABI}/libbytehook.so  ($(du -sh "${SO_SRC}" | cut -f1))"
        INSTALLED_COUNT=$(( INSTALLED_COUNT + 1 ))
    else
        echo "[vendor] WARNING: libbytehook.so not found for ABI=${ABI} in AAR — skipping."
    fi
done

if [[ "${INSTALLED_COUNT}" -eq 0 ]]; then
    echo "[vendor] FATAL: No libbytehook.so found for any ABI inside the AAR."
    echo "[vendor] All .so files found:"
    find "${EXTRACT_DIR}" -name "*.so" | sed 's#^#  #' || true
    exit 1
fi

# ── Final verification ─────────────────────────────────────────────────────────
if [[ ! -f "${VENDOR_DIR}/include/bytehook.h" ]]; then
    echo "[vendor] FATAL: Header verification failed after copy."
    exit 1
fi

echo "[vendor] ── Vendor complete ──────────────────────────────────"
find "${VENDOR_DIR}" -type f | sort | sed 's#^#  #'
echo "[vendor] ─────────────────────────────────────────────────────"
