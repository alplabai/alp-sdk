/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static-link probe for the Linux camera backend.  Two ctest rules use it:
 * `nm` must list the backend's registry entry in this binary (the archive
 * member was linked), and running it must show the dispatcher selected that
 * backend: an alias that does not exist is ALP_ERR_NOT_READY from yocto_drv,
 * while the zephyr_stub would answer ALP_ERR_NOT_IMPLEMENTED.  No camera
 * hardware is touched.
 */

#include <alp/camera.h>
#include <alp/peripheral.h>

int main(void)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(987654u);
	cfg.width               = 320u;
	cfg.height              = 240u;
	cfg.format              = ALP_PIXFMT_GREY8;
	if (alp_camera_open(&cfg) != NULL) return 2;
	return alp_last_error() == ALP_ERR_NOT_READY ? 0 : 1;
}
