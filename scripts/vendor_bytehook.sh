#!/usr/bin/env bash
# Vendors ByteHook into libs/bytehook/
# Usage:
#   bash scripts/vendor_bytehook.sh
#   bash scripts/vendor_bytehook.sh <branch|tag|commit>
#
# If no ref is provided, the script clones the default branch.

set -euo pipefail

BYTEHOOK_REF="${1:-}"
REPO_URL="https://github.com/bytedance/bhook.git"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${ROOT_DIR}/libs/bytehook"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

mkdir -p "${VENDOR_DIR}"

echo "[vendor] Cloning ByteHook repository..."
if [[ -n "${BYTEHOOK_REF}" ]]; then
  echo "[vendor] Requested ref: ${BYTEHOOK_REF}"
  if git ls-remote --exit-code --heads "${REPO_URL}" "${BYTEHOOK_REF}" >/dev/null 2>&1 \
     || git ls-remote --exit-code --tags  "${REPO_URL}" "${BYTEHOOK_REF}" >/dev/null 2>&1; then
    git clone --depth 1 --branch "${BYTEHOOK_REF}" "${REPO_URL}" "${TMP_DIR}/bhook"
  else
    echo "[vendor] WARNING: ref '${BYTEHOOK_REF}' not found. Cloning default branch instead."
    git clone --depth 1 "${REPO_URL}" "${TMP_DIR}/bhook"
  fi
else
  git clone --depth 1 "${REPO_URL}" "${TMP_DIR}/bhook"
fi

CLONE_DIR="${TMP_DIR}/bhook"

if [[ ! -d "${CLONE_DIR}" ]]; then
  echo "[vendor] FATAL: clone failed — ${CLONE_DIR} not found"
  exit 1
fi

echo "[vendor] Resolving header path..."
HEADER_SRC="$(find "${CLONE_DIR}" -type f -name "bytehook.h" | head -n1 || true)"
if [[ -z "${HEADER_SRC}" ]]; then
  echo "[vendor] FATAL: bytehook.h not found."
  echo "[vendor] Repo tree (headers only):"
  find "${CLONE_DIR}" -type f -name "*.h" | sed 's#^#  #'
  exit 1
fi

mkdir -p "${VENDOR_DIR}/include"
cp -f "${HEADER_SRC}" "${VENDOR_DIR}/include/bytehook.h"
echo "[vendor] Installed header: ${VENDOR_DIR}/include/bytehook.h"

echo "[vendor] Resolving prebuilts..."
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
  SO_SRC="$(find "${CLONE_DIR}" -type f -path "*/${ABI}/libbytehook.so" | head -n1 || true)"
  if [[ -n "${SO_SRC}" ]]; then
    mkdir -p "${VENDOR_DIR}/${ABI}"
    cp -f "${SO_SRC}" "${VENDOR_DIR}/${ABI}/libbytehook.so"
    echo "[vendor] Installed ${ABI}/libbytehook.so"
  else
    echo "[vendor] WARNING: libbytehook.so not found for ABI=${ABI} — skipping."
  fi
done

if [[ ! -f "${VENDOR_DIR}/include/bytehook.h" ]]; then
  echo "[vendor] FATAL: Header copy failed."
  exit 1
fi

echo "[vendor] Done."
echo "[vendor] Header: $(ls -lh "${VENDOR_DIR}/include/bytehook.h")"
find "${VENDOR_DIR}" -maxdepth 2 -type f | sed 's#^#  #'
