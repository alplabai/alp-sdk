/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cc3501e.h
 * @brief Wire protocol between Alif Ensemble and the on-module
 *        TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor.
 *
 * The CC3501E ships its own Cortex-M MCU and runs ALP-authored
 * firmware (separate project: `alplabai/cc3501e-firmware`).  The
 * firmware exposes Wi-Fi + BLE control to the Alif over the
 * inter-chip SPI1 bus -- Alif is master, CC3501E is slave.  This
 * header is the contract between the two sides: any change here
 * must be matched in the firmware's parser.
 *
 * Why a custom protocol instead of standardised Wi-Fi-host
 * commands (e.g. ESP-AT)?  We need granular control over the BLE
 * GATT path + the GPIO proxy (the CC3501E drives IO11 / IO13 /
 * IO15..IO21 + the camera-enable LDOs); ESP-AT is Wi-Fi-only.
 * The protocol is intentionally small and binary -- no AT
 * tokenisation, no escape sequences -- because the channel is a
 * hardwired SPI bus, not a UART-on-headphone-jack.
 *
 * Frame format (little-endian where applicable):
 *
 *   +--------+--------+--------+--------+========+========+
 *   |  cmd   |  flags | payload_len_lo  | payload (N B)   |
 *   +--------+--------+--------+--------+========+========+
 *
 *   cmd          one of `alp_cc3501e_cmd_t`
 *   flags        bit 0 = response-required
 *                bit 1 = async-event payload (slave -> master)
 *                bits 2..7 reserved
 *   payload_len  16-bit LE; total frame = 4 + payload_len
 *
 * Responses use the same shape -- the slave sets flags bit 1
 * (async event) for unsolicited notifications (incoming
 * advertisement, BLE connection event, Wi-Fi disconnect) so the
 * Alif's SPI master can demux solicited replies from events on
 * the same MISO line.
 */

#ifndef ALP_PROTOCOL_CC3501E_H
#define ALP_PROTOCOL_CC3501E_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALP_CC3501E_PROTOCOL_VERSION 1

/** Frame header in bytes, before the payload. */
#define ALP_CC3501E_HEADER_BYTES 4

/** Maximum payload size per frame.  Larger transactions must split
 *  across multiple frames using the FRAME_CONTINUATION flag (bit 2,
 *  reserved in v1; v2 will land alongside the BLE long-write path). */
#define ALP_CC3501E_MAX_PAYLOAD 512

/** Flags bitfield. */
#define ALP_CC3501E_FLAG_RESP_REQUIRED 0x01
#define ALP_CC3501E_FLAG_ASYNC_EVENT 0x02

/**
 * @brief Command opcodes.
 *
 * Numbering is grouped by subsystem so additions don't perturb the
 * existing range and so the firmware's dispatch table stays sparse-
 * friendly:
 *
 *   0x00..0x0F  meta (ping, version, reset)
 *   0x10..0x2F  Wi-Fi
 *   0x30..0x4F  BLE
 *   0x50..0x5F  GPIO proxy
 *   0x60..0x6F  power / camera-enable
 *   0x70..0x7F  diagnostics
 *   0x80..0xFF  reserved (vendor extensions)
 */
typedef enum {
    /* Meta */
    ALP_CC3501E_CMD_PING        = 0x00,
    ALP_CC3501E_CMD_GET_VERSION = 0x01,
    ALP_CC3501E_CMD_RESET       = 0x02,
    ALP_CC3501E_CMD_GET_MAC     = 0x03,

    /* Wi-Fi */
    ALP_CC3501E_CMD_WIFI_SCAN_START   = 0x10,
    ALP_CC3501E_CMD_WIFI_SCAN_STOP    = 0x11,
    ALP_CC3501E_CMD_WIFI_CONNECT_STA  = 0x12,
    ALP_CC3501E_CMD_WIFI_DISCONNECT   = 0x13,
    ALP_CC3501E_CMD_WIFI_AP_START     = 0x14,
    ALP_CC3501E_CMD_WIFI_AP_STOP      = 0x15,
    ALP_CC3501E_CMD_WIFI_GET_RSSI     = 0x16,
    ALP_CC3501E_CMD_WIFI_GET_IP       = 0x17,
    ALP_CC3501E_EVT_WIFI_SCAN_RESULT  = 0x18, /* async, slave -> master */
    ALP_CC3501E_EVT_WIFI_CONNECTED    = 0x19, /* async */
    ALP_CC3501E_EVT_WIFI_DISCONNECTED = 0x1A, /* async */

    /* TCP/UDP sockets (host-managed; offload to firmware). */
    ALP_CC3501E_CMD_SOCK_OPEN    = 0x20,
    ALP_CC3501E_CMD_SOCK_CONNECT = 0x21,
    ALP_CC3501E_CMD_SOCK_SEND    = 0x22,
    ALP_CC3501E_CMD_SOCK_RECV    = 0x23,
    ALP_CC3501E_CMD_SOCK_CLOSE   = 0x24,

    /* BLE */
    ALP_CC3501E_CMD_BLE_ENABLE         = 0x30,
    ALP_CC3501E_CMD_BLE_DISABLE        = 0x31,
    ALP_CC3501E_CMD_BLE_ADV_START      = 0x32,
    ALP_CC3501E_CMD_BLE_ADV_STOP       = 0x33,
    ALP_CC3501E_CMD_BLE_SCAN_START     = 0x34,
    ALP_CC3501E_CMD_BLE_SCAN_STOP      = 0x35,
    ALP_CC3501E_CMD_BLE_CONNECT        = 0x36,
    ALP_CC3501E_CMD_BLE_DISCONNECT     = 0x37,
    ALP_CC3501E_CMD_BLE_GATT_REGISTER  = 0x38,
    ALP_CC3501E_CMD_BLE_GATT_NOTIFY    = 0x39,
    ALP_CC3501E_CMD_BLE_GATT_READ      = 0x3A,
    ALP_CC3501E_CMD_BLE_GATT_WRITE     = 0x3B,
    ALP_CC3501E_EVT_BLE_ADV_REPORT     = 0x3C, /* async */
    ALP_CC3501E_EVT_BLE_CONNECTED      = 0x3D, /* async */
    ALP_CC3501E_EVT_BLE_DISCONNECTED   = 0x3E, /* async */
    ALP_CC3501E_EVT_BLE_GATT_WRITE_REQ = 0x3F, /* async */

    /* GPIO proxy.  IO11 / IO13 / IO15..IO21 hang off CC3501E
     * GPIOs; these commands let the Alif read/write them via the
     * inter-chip bus. */
    ALP_CC3501E_CMD_GPIO_CONFIGURE     = 0x50,
    ALP_CC3501E_CMD_GPIO_WRITE         = 0x51,
    ALP_CC3501E_CMD_GPIO_READ          = 0x52,
    ALP_CC3501E_CMD_GPIO_SET_INTERRUPT = 0x53,
    ALP_CC3501E_EVT_GPIO_INTERRUPT     = 0x54, /* async */

    /* Power / camera enables.  CC3501E drives the camera-LDO
     * enable pins (CAM_EN_LDO0/1) per the inter-chip TSV. */
    ALP_CC3501E_CMD_CAM_ENABLE  = 0x60,
    ALP_CC3501E_CMD_CAM_DISABLE = 0x61,

    /* Diagnostics */
    ALP_CC3501E_CMD_DIAG_GET_STATS = 0x70,
    ALP_CC3501E_CMD_DIAG_LOG_LEVEL = 0x71,
} alp_cc3501e_cmd_t;

/**
 * @brief Response status codes carried in the first byte of every
 *        response payload.  Maps cleanly onto the SDK's alp_status_t
 *        when the host adapts the value.
 */
typedef enum {
    ALP_CC3501E_RESP_OK            = 0x00,
    ALP_CC3501E_RESP_ERR_INVALID   = 0x01, /**< Bad cmd / bad payload. */
    ALP_CC3501E_RESP_ERR_BUSY      = 0x02, /**< Subsystem in use. */
    ALP_CC3501E_RESP_ERR_TIMEOUT   = 0x03,
    ALP_CC3501E_RESP_ERR_NO_MEM    = 0x04,
    ALP_CC3501E_RESP_ERR_NOT_READY = 0x05, /**< Wi-Fi/BLE not enabled. */
    ALP_CC3501E_RESP_ERR_RADIO     = 0x06, /**< RF / antenna failure. */
    ALP_CC3501E_RESP_ERR_PROTOCOL  = 0x07, /**< Frame mis-parse. */
    ALP_CC3501E_RESP_ERR_VERSION   = 0x08, /**< Firmware ↔ host version mismatch. */
    ALP_CC3501E_RESP_ERR_INTERNAL  = 0xFF
} alp_cc3501e_resp_t;

/* ------------------------------------------------------------------ */
/* Wi-Fi STA payload formats                                          */
/* ------------------------------------------------------------------ */

/** Sent in the payload of CMD_WIFI_CONNECT_STA.  ssid_len + psk_len
 *  upper-bound on the cumulative frame length (still ≤ MAX_PAYLOAD). */
typedef struct {
    uint8_t ssid_len;
    uint8_t psk_len;
    uint8_t security; /**< 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE */
    uint8_t reserved;
    /* uint8_t ssid[ssid_len];   -- packed inline, no padding */
    /* uint8_t psk[psk_len];     -- packed inline, no padding */
} alp_cc3501e_wifi_connect_t;

/** Async event for CMD_WIFI_SCAN_START and friends. */
typedef struct {
    uint8_t bssid[6];
    int8_t  rssi_dbm;
    uint8_t channel;
    uint8_t security;
    uint8_t ssid_len;
    /* uint8_t ssid[ssid_len]; */
} alp_cc3501e_scan_result_t;

/* ------------------------------------------------------------------ */
/* BLE advertising / scanning payload formats                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  connectable;
    uint8_t  reserved;
    uint16_t interval_min_ms;
    uint16_t interval_max_ms;
    uint8_t  adv_data_len;
    /* uint8_t adv_data[adv_data_len]; */
} alp_cc3501e_ble_adv_start_t;

typedef struct {
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t  rssi_dbm;
    uint8_t adv_type;
    uint8_t adv_data_len;
    /* uint8_t adv_data[adv_data_len]; */
} alp_cc3501e_ble_adv_report_t;

/* ------------------------------------------------------------------ */
/* GPIO proxy payload formats                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t cc3501e_gpio; /**< CC3501E pad index (e.g. 13 for GPIO13). */
    uint8_t direction;    /**< 0 = input, 1 = output */
    uint8_t pull;         /**< 0 = none, 1 = up, 2 = down */
    uint8_t reserved;
} alp_cc3501e_gpio_configure_t;

typedef struct {
    uint8_t cc3501e_gpio;
    uint8_t level; /**< 0 or 1 */
    uint8_t reserved[2];
} alp_cc3501e_gpio_write_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PROTOCOL_CC3501E_H */
