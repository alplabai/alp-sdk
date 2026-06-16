/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-board-id-readout — read the SoM's hardware-info manifest
 * from the on-module 24C128 EEPROM and print every field.
 *
 * Most production firmware runs this at boot then calls
 * alp_hw_info_assert_matches_build() to halt if the SKU / hw_rev
 * disagrees with the build's expected target -- this catches the
 * "wrong firmware on this board" class of incident before the
 * application code has a chance to mis-configure anything.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/hw_info.h"

int main(void)
{
	printf("[board-id] read SoM hardware-info manifest\n");

	/* alp_hw_info_read fills in family / SKU / hw_rev / serial +
     * manufacturing date from the EEPROM manifest.  The Kconfig
     * symbols CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID +
     * CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT must be set to the
     * board's wiring (E1M_I2C0 on V2N; 0x50 strap default). */
	alp_hw_info_t info;
	alp_status_t  s = alp_hw_info_read(&info);

	switch (s) {
	case ALP_OK:
		printf("[board-id] family    = %s\n", info.som_family);
		printf("[board-id] sku       = %s\n", info.som_sku);
		printf("[board-id] hw_rev    = %s\n", info.som_hw_rev);
		printf("[board-id] serial    = %s\n", info.som_serial);
		printf("[board-id] mfg date  = %04u-%02u-%02u\n",
		       info.som_mfg_year,
		       info.som_mfg_month,
		       info.som_mfg_day);
		break;
	case ALP_ERR_NOSUPPORT:
		printf("[board-id] EEPROM bus not configured\n");
		printf("[board-id]   set CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID\n");
		printf("[board-id]   in prj.conf to the bus id matching\n");
		printf("[board-id]   the SoM's E1M_I2C0 alias.\n");
		break;
	case ALP_ERR_NOT_PROVISIONED:
		printf("[board-id] EEPROM is blank -- module not yet provisioned\n");
		printf("[board-id]   run scripts/program_eeprom.py at production\n");
		printf("[board-id]   test to write the SoM manifest.\n");
		break;
	case ALP_ERR_IO:
		printf("[board-id] manifest is corrupt (bad schema or CRC)\n");
		printf("[board-id]   magic is present but the body failed its\n");
		printf("[board-id]   integrity check; flag for rework.\n");
		break;
	case ALP_ERR_NOT_READY:
		printf("[board-id] EEPROM not reachable on the bus\n");
		printf("[board-id]   double-check the EEPROM is populated and\n");
		printf("[board-id]   the strap address (0x50 default) matches.\n");
		break;
	default:
		printf("[board-id] alp_hw_info_read -> %d\n", (int)s);
		break;
	}

	/* Optional second pass: assert the build is for the right board.
     * Pass NULL for fields that don't matter (e.g. cross-rev
     * firmware can pass NULL for hw_rev). */
	if (s == ALP_OK) {
		s = alp_hw_info_assert_matches_build(&info,
		                                     /* expected_sku */ "E1M-V2N101",
		                                     /* expected_hw_rev */ NULL);
		if (s == ALP_OK) {
			printf("[board-id] firmware build is for this SoM family\n");
		} else {
			printf("[board-id] WARNING: build expected sku 'E1M-V2N101' "
			       "but module reports '%s'\n",
			       info.som_sku);
		}
	}

	printf("[board-id] done\n");
	return 0;
}
