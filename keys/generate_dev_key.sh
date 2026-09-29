#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Generates YOUR OWN ECDSA-P256 development signing key for MCUboot.
# Idempotent: skips generation if the target file already exists.
#
# Output: keys/mcuboot_dev_ecdsa_p256.pem (gitignored)
#
# Optional: the default build signs with the committed SHARED dev key
# (keys/mcuboot_shared_dev_ecdsa_p256.pem, #2421), which the factory
# MCUboot on pre-provisioned modules trusts.  A key made here is only
# accepted by an MCUboot you build with it and re-provision -- see
# keys/README.md, which also covers the production key lifecycle.
#
#   bash keys/generate_dev_key.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEY_PATH="${SCRIPT_DIR}/mcuboot_dev_ecdsa_p256.pem"

if [ -f "${KEY_PATH}" ]; then
    echo "keys/generate_dev_key.sh: ${KEY_PATH} already exists; nothing to do."
    echo "  (delete it first if you want to rotate the dev key.)"
    exit 0
fi

# imgtool is the canonical MCUboot key-management CLI; ships in
# zephyrproject-rtos/mcuboot/scripts/imgtool.py and is on $PATH
# once Zephyr's requirements.txt is installed (pip3 install
# imgtool).  Prefer the imgtool entry point so the key format
# matches what MCUboot's sysbuild step expects.
if ! command -v imgtool >/dev/null 2>&1; then
    echo "keys/generate_dev_key.sh: imgtool not found on PATH." >&2
    echo "  Install via 'pip3 install imgtool' or activate the Zephyr Python venv." >&2
    exit 1
fi

echo "Generating ECDSA-P256 dev signing key at ${KEY_PATH}..."
imgtool keygen --key "${KEY_PATH}" --type ecdsa-p256

# Tighten permissions -- a leaked dev key still confuses the
# device-trust story even if it has no production reach.
chmod 600 "${KEY_PATH}"

echo "Done.  Build with -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=${KEY_PATH} and re-provision MCUboot:"
echo "  the factory MCUboot trusts only keys/mcuboot_shared_dev_ecdsa_p256.pem."
echo "REMINDER: this key is for development only.  See keys/README.md."
