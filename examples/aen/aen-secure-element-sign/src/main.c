/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-secure-element-sign -- exercise the OPTIGA Trust M secure
 * element on the E1M-AEN (Alif Ensemble) SoM by initialising it,
 * reading the product info object, and issuing an ECDSA-P256 sign
 * APDU against a fixed message hash.
 *
 * On the E1M-AEN the Trust M sits on **BRD_I2C** -- the on-module
 * housekeeping bus (the Alif LPI2C0 / "LP-island" I2C, P7_4 SCL_A /
 * P7_5 SDA_A) -- at 7-bit address 0x30, alongside the RTC + EEPROM +
 * TMP112.  Because BRD_I2C lives in the low-power domain it is owned
 * by the **M55-HE** subsystem (hence this example's board target is
 * rtss_he).  The chip and the sign flow are identical to the V2N
 * variant -- everything goes through the SoM-portable <alp/...> API,
 * so the only AEN-specific facts are the bus + the owning core.
 *
 * On a fresh chip the host has to (1) issue OPEN_APPLICATION at the
 * data-link layer -- the driver's `optiga_trust_m_init` does this --
 * and (2) issue a CALC_SIGN APDU referencing the key slot the
 * production line provisioned with an ECC private key.
 *
 * The example targets **key OID 0xE0F0** which is the Trust M's
 * canonical "Device Endpoint" device-identity slot per Infineon's
 * Solution Reference Manual; chips that aren't provisioned (e.g. a
 * factory-fresh part on an early-bring-up board) reply with status
 * `0x01 0x02` ("data-object not present") and the example prints the
 * failure so the operator can see whether provisioning has run.
 *
 * Hash input: the SHA-256 of the ASCII string "alp" is hard-coded as
 * the message digest so a reader can recompute it independently.
 * Real apps pass a freshly-computed digest from <alp/security.h>'s
 * alp_hash_* surface.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/optiga_trust_m.h"

/* Message digest the host wants the secure element to sign.  This is
 * SHA-256("alp") = 11c95710cad928b86c5b16fa3957cda7c7ce7eed1d3d3d59
 *                   59586a8e76b6e9c0  (32 bytes).
 * Hard-coded so the example output is reproducible without pulling
 * in the SHA-256 wrapper. */
static const uint8_t MESSAGE_DIGEST[32] = {
    0x11, 0xc9, 0x57, 0x10, 0xca, 0xd9, 0x28, 0xb8, 0x6c, 0x5b, 0x16, 0xfa, 0x39, 0x57, 0xcd, 0xa7,
    0xc7, 0xce, 0x7e, 0xed, 0x1d, 0x3d, 0x3d, 0x59, 0x59, 0x58, 0x6a, 0x8e, 0x76, 0xb6, 0xe9, 0xc0,
};

/* CALC_SIGN APDU for the OPTIGA Trust M, per Infineon's SRM table 16.
 *   Cmd byte:    0x31 (CalcSign)
 *   Param byte:  0x11 (ECDSA over SHA-256 digest)
 *   InLen[2]:    BE-uint16 length of the InData block
 *   InData:      Tag 0x01 | Len[2] | digest[32] | Tag 0x03 | Len[2] | OID[2]
 *   The OID is BE -- key slot 0xE0F0 = bytes 0xE0 0xF0 in that order. */
static size_t build_calc_sign_apdu(uint8_t *out, size_t cap)
{
    /* Inside InData: Tag(0x01) + Len(2) + digest(32) + Tag(0x03)
     * + Len(2) + OID(2)  =  1 + 2 + 32 + 1 + 2 + 2  = 40 bytes. */
    const uint16_t in_data_len = 1u + 2u + 32u + 1u + 2u + 2u;
    /* APDU = 4-byte header + in_data_len.  We need cap >= header + body. */
    const size_t total = 4u + (size_t)in_data_len;
    if (out == NULL || cap < total) return 0u;

    out[0] = 0x31u; /* CalcSign command id */
    out[1] = 0x11u; /* ECDSA(SHA-256 digest) */
    out[2] = (uint8_t)((in_data_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(in_data_len & 0xFFu);

    /* InData block. */
    size_t off = 4u;
    out[off++] = 0x01u; /* Tag: digest */
    out[off++] = 0x00u; /* Len high (LSB later) */
    out[off++] = 32u;   /* Len low */
    memcpy(&out[off], MESSAGE_DIGEST, 32u);
    off += 32u;

    out[off++] = 0x03u; /* Tag: key OID reference */
    out[off++] = 0x00u; /* Len high */
    out[off++] = 2u;    /* Len low */
    out[off++] = 0xE0u; /* OID byte 0 (key slot) */
    out[off++] = 0xF0u; /* OID byte 1 */

    return off; /* MUST equal `total`. */
}

int main(void)
{
    printf("[se] aen-secure-element-sign\n");

    /* BRD_I2C carries the Trust M alongside the RTC + EEPROM + TMP112.
     * On the E1M-AEN this is the Alif LPI2C0 (the LP-island I2C),
     * surfaced as portable bus 0.  400 kHz is the standard Trust M bus
     * rate; the chip supports up to 1 MHz Fast-mode+ if the rest of
     * the bus does too. */
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,
        .bitrate_hz = 400000u,
    });
    if (bus == NULL) {
        printf("[se] alp_i2c_open failed: %d\n", (int)alp_last_error());
        return 0;
    }

    /* Init the driver.  This issues OPEN_APPLICATION at the chip's
     * data-link layer; the chip replies with its capability mask. */
    optiga_trust_m_t se;
    alp_status_t     s = optiga_trust_m_init(&se, bus, OPTIGA_TRUST_M_I2C_ADDR);
    if (s != ALP_OK) {
        printf("[se] optiga_trust_m_init -> %d  (chip not on bus?)\n", (int)s);
        alp_i2c_close(bus);
        return 0;
    }

    /* Read the product info object before issuing the sign.  This
     * is a sanity check that the chip is responding to APDUs (vs
     * just ACKing the I2C address) and that the firmware build on
     * the chip matches what production-test signed off on. */
    optiga_trust_m_product_info_t info;
    s = optiga_trust_m_read_product_info(&se, &info);
    if (s == ALP_OK) {
        printf("[se] product info: chip_type=%02X%02X%02X%02X%02X%02X "
               "fw_id=%02X%02X build=%02X%02X\n",
               info.chip_type[0], info.chip_type[1], info.chip_type[2], info.chip_type[3],
               info.chip_type[4], info.chip_type[5], info.fw_id[0], info.fw_id[1], info.fw_build[0],
               info.fw_build[1]);
    } else {
        printf("[se] read_product_info -> %d  (continuing -- sign may still work)\n", (int)s);
    }

    /* Build the CalcSign APDU.  The buffer is sized for the largest
     * APDU we generate (44 bytes); doubling that gives headroom if
     * the example grows. */
    uint8_t      apdu[128];
    const size_t apdu_len = build_calc_sign_apdu(apdu, sizeof apdu);
    if (apdu_len == 0u) {
        printf("[se] build_calc_sign_apdu produced empty frame\n");
        optiga_trust_m_deinit(&se);
        alp_i2c_close(bus);
        return 0;
    }

    /* Issue the APDU.  ECDSA over P-256 produces a max-72-byte
     * ASN.1-encoded signature (two 33-byte INTEGERs + sequence
     * overhead); 96 bytes of reply buffer is comfortable.  The 1s
     * timeout covers the chip's ~50 ms worst-case CalcSign latency
     * with margin for I2C clock-stretching. */
    uint8_t resp[96];
    size_t  resp_len = 0u;
    s = optiga_trust_m_send_apdu(&se, apdu, apdu_len, resp, sizeof resp, &resp_len, 1000u);
    if (s != ALP_OK) {
        printf("[se] send_apdu(CalcSign) -> %d\n", (int)s);
    } else if (resp_len < 4u) {
        printf("[se] response too short: %u bytes\n", (unsigned)resp_len);
    } else {
        /* APDU response layout:
         *   resp[0] = StaCode  (0x00 = OK; non-zero on error)
         *   resp[1] = UndefByte
         *   resp[2..3] = OutLen (BE-uint16, signature byte count)
         *   resp[4..] = ASN.1 DER signature              */
        const uint8_t  stacode = resp[0];
        const uint16_t outlen  = (uint16_t)((resp[2] << 8) | resp[3]);
        printf("[se] CalcSign reply: stacode=0x%02X  outlen=%u  total=%u\n", stacode,
               (unsigned)outlen, (unsigned)resp_len);
        if (stacode == 0u && outlen + 4u == resp_len) {
            /* Print the first 16 bytes of the signature as a sanity
             * check.  Real apps either feed the signature directly
             * into a TLS handshake / attestation payload or store
             * it alongside the firmware blob for downstream
             * verification.  */
            printf("[se] signature[0..15]: ");
            const size_t show = outlen < 16u ? outlen : 16u;
            for (size_t i = 0u; i < show; ++i)
                printf("%02X", resp[4u + i]);
            printf("\n");
        } else if (stacode != 0u) {
            /* Common: 0x01 0x02 = "data object referenced does not
             * exist" (key slot not provisioned).  See SRM table 17
             * for the full status-code table. */
            printf("[se] chip reported error; check production "
                   "provisioning for key slot 0xE0F0\n");
        }
    }

    optiga_trust_m_deinit(&se);
    alp_i2c_close(bus);
    printf("[se] done\n");
    return 0;
}
