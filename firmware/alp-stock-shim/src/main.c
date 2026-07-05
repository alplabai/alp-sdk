/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

int main(void)
{
	printk("alp-stock-shim: idle peer core online\n");

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
