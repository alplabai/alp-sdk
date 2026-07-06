/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke test — confirms the public Alp SDK headers compile and link
 * against the configured backend.  Each peripheral wrapper will land
 * its own dedicated test alongside its implementation.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/display.h"
#include "alp/camera.h"
#include "alp/gui.h"
#include "alp/iot.h"
#include "alp/version.h"

int main(void)
{
	printf("Alp SDK smoke: headers compiled and linked (SDK v%s).\n", alp_version_string());
	return 0;
}
