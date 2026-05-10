/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke test — confirms the public ALP SDK headers compile and link
 * against the configured backend.  Each peripheral wrapper will land
 * its own dedicated test alongside its implementation.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/display.h"
#include "alp/camera.h"
#include "alp/gui.h"
#include "alp/iot.h"

int main(void) {
    printf("ALP SDK smoke: headers compiled and linked.\n");
    return 0;
}
