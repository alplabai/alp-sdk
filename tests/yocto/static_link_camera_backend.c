/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link-only probe: references the camera dispatcher from a STATIC libalp_sdk.a
 * so the ctest rule next to it can look for the Linux
 * V4L2 backend's registry entry in the final binary.  It never runs a camera.
 */

#include <alp/camera.h>

int main(void)
{
	alp_camera_config_t cfg = { 0 };
	/* width/height 0 is rejected before any device is touched. */
	return alp_camera_open(&cfg) == NULL ? 0 : 1;
}
