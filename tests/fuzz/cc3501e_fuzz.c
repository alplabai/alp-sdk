/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * libFuzzer harness for the CC3501E host driver's reply decoder,
 * `cc3501e_reply_verdict()` in `chips/cc3501e/cc3501e_core.c`: the function
 * `cc3501e_request()` runs on every reply read off the SPI wire (status byte,
 * the proto-4 CRC-16/CCITT-FALSE trailer, the all-zero dead-phase guard).
 *
 * The harness links the real cc3501e_core.c and stubs only the bus, GPIO and
 * time seams that file references.  Input layout:
 *
 *   byte[0]    command opcode
 *   byte[1]    fw_proto_major; bit 7 set = patch a valid CRC trailer onto the
 *              payload first (a random buffer never carries one, so without
 *              this the proto-4 path past the CRC check is unreachable)
 *   byte[2..5] reply header
 *   byte[6..]  reply payload
 *
 * Build:
 *   cmake -B build-fuzz -DALP_BUILD_FUZZ=ON -DALP_OS=yocto \
 *         -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz --target alp_fuzz_cc3501e
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alp/protocol/crc16.h"
#include "cc3501e_internal.h"

/* Seams cc3501e_core.c references but cc3501e_reply_verdict() never reaches. */
void alp_delay_ms(uint32_t ms)
{
	(void)ms;
}

void alp_delay_us(uint32_t us)
{
	(void)us;
}

uint64_t alp_uptime_ms(void)
{
	return 0u;
}

uint64_t alp_uptime_us(void)
{
	return 0u;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	(void)pin;
	(void)level;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_spi_transceive(alp_spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
	(void)spi;
	(void)tx;
	(void)rx;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t cc3501e_ping(cc3501e_t *ctx)
{
	(void)ctx;
	return ALP_ERR_NOSUPPORT;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	enum { PREFIX = 2 + ALP_CC3501E_HEADER_BYTES };
	if (size < PREFIX) return 0;

	const alp_cc3501e_cmd_t cmd   = (alp_cc3501e_cmd_t)data[0];
	const uint8_t           major = data[1] & 0x7Fu;
	const uint8_t          *hdr   = data + 2;

	uint8_t payload[1024];
	size_t  plen = size - PREFIX;
	if (plen > sizeof(payload)) plen = sizeof(payload);
	memcpy(payload, data + PREFIX, plen);

	if ((data[1] & 0x80u) != 0u && plen >= ALP_CC3501E_CRC_BYTES) {
		const size_t covered = plen - ALP_CC3501E_CRC_BYTES;
		uint16_t     crc     = alp_crc16_ccitt_false(hdr, ALP_CC3501E_HEADER_BYTES);
		crc                  = alp_crc16_ccitt_false_update(crc, payload, covered);
		payload[covered]     = (uint8_t)(crc & 0xFFu);
		payload[covered + 1] = (uint8_t)(crc >> 8);
	}

	(void)cc3501e_reply_verdict(cmd, major, hdr, payload, (uint16_t)plen);
	(void)cc3501e_reply_verdict(cmd, major, hdr, NULL, (uint16_t)plen);
	(void)cc3501e_reply_may_be_all_zero(cmd);
	(void)cc3501e_fw_major_is_acceptable(major);
	return 0;
}
