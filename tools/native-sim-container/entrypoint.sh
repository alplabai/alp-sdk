#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Entrypoint for the native_sim reproduction container (Containerfile).
#
# With NO arguments it runs the exact twister invocation that
# .github/workflows/pr-twister.yml runs in CI, against the alp-sdk
# checkout bind-mounted at $EXTRA_ZEPHYR_MODULES.  With arguments it
# execs them verbatim (e.g. `... alp-native-sim bash` for a shell, or a
# custom `python3 zephyr/scripts/twister ...` line for a narrower run).
set -euo pipefail

ALP_SDK="${EXTRA_ZEPHYR_MODULES:-/work/alp-sdk}"

if [ ! -e "${ALP_SDK}/west.yml" ]; then
	echo "error: alp-sdk not found at ${ALP_SDK}" >&2
	echo "       bind-mount the repo, e.g.:" >&2
	echo "         podman run --rm -v \"\$PWD\":/work/alp-sdk:z alp-native-sim" >&2
	exit 2
fi

# Custom command requested -> run it inside the workspace (ZEPHYR_BASE /
# EXTRA_ZEPHYR_MODULES already exported by the image).
if [ "$#" -gt 0 ]; then
	exec "$@"
fi

# Default: freeze pr-twister.yml's twister step.  Run from the baked
# Zephyr workspace; testsuite-roots point into the mounted alp-sdk.
cd "${ZEPHYR_BASE%/zephyr}"
exec python3 zephyr/scripts/twister \
	--testsuite-root "${ALP_SDK}/tests/unit" \
	--testsuite-root "${ALP_SDK}/tests/zephyr" \
	--testsuite-root "${ALP_SDK}/examples" \
	-p native_sim/native/64 \
	--inline-logs \
	--no-detailed-test-id
