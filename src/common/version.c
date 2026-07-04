/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime accessor for the SDK release version.  Pure .rodata; the
 * value comes from <alp/version.h>, which scripts/bump_version.py
 * keeps in lockstep with metadata/sdk_version.yaml.
 */

#include <alp/version.h>

const char *alp_version_string(void)
{
	return ALP_VERSION_STRING;
}
