/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alp_e1m_evk_aen.h
 * @brief Carrier-feature names for the E1M EVK populated with an
 *        E1M-AEN family SoM (UG-E1M-001).
 *
 * Layered atop the E1M-standard fixed pinout in
 * `<alp/e1m_pinout.h>`.  This header only defines names for things
 * the standard does NOT name -- the EVK's RGB LED, rotary encoder,
 * IO-expander pins, on-board sensor I2C addresses, and the
 * carrier's repurposing of certain SoM pads (e.g. SPI0 pins as
 * GPIOs for amp / IO-expander control).
 *
 * For the underlying integers -- bus / port instance IDs and
 * pad-level GPIO indices -- `#include <alp/e1m_pinout.h>` directly.
 */

#ifndef ALP_BOARDS_ALP_E1M_EVK_AEN_H
#define ALP_BOARDS_ALP_E1M_EVK_AEN_H

#include "alp/e1m_pinout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* RGB LED (PWM channels)                                             */
/*                                                                    */
/* The EVK drives the three RGB LED channels through PWM, not as      */
/* simple GPIOs -- this gives the studio's UI block proper            */
/* brightness control out of the box.  Use alp_pwm_open() with        */
/* these channel IDs.                                                 */
/* ================================================================== */

#define EVK_AEN_PWM_LED_RED ALP_E1M_PWM3 /**< RGB LED red  channel. */
#define EVK_AEN_PWM_LED_GREEN                                                                      \
    ALP_E1M_PWM0 /**< RGB LED green channel.  Note: not contiguous with R/B -- the EVK schematic wires green via PWM0, not PWM2. */
#define EVK_AEN_PWM_LED_BLUE ALP_E1M_PWM1 /**< RGB LED blue channel. */

/* ================================================================== */
/* Direct GPIO mappings (E1M-standard pads)                           */
/* ================================================================== */

#define EVK_AEN_PIN_CAM_MUX_SEL                                                                    \
    ALP_E1M_GPIO_IO2 /**< PI3WVR626 SEL pin. See `evk_aen_cam_select_*` enum below + chips/cam_mux_pi3wvr626. */
/** Rotary encoder push switch (PEC12R-4222F-S0024 SW pin, R92 10k
 *  pull-up to VIO, 0.1uF debounce cap to GND, active-low when
 *  pressed).  Maps to E1M IO4. */
#define EVK_AEN_PIN_ENCODER_SW ALP_E1M_GPIO_IO4
#define EVK_AEN_PIN_CAM_RST                                                                        \
    ALP_E1M_GPIO_IO5 /**< Camera reset (was previously misdocumented as IO_EXP_RST -- the schematic confirms IO5 -> CAM_RST and the I/O expander reset is on a different pad; see below). */

/* SDIO 74LVC157 multiplexer (M.2 E-key SDIO vs microSD card slot).
 *
 * Two 74LVC157 quad 2:1 muxes (U38 for data lines, U39 for CLK/CMD/RST)
 * pick which device drives the SoM's single SDIO bus.  Control pins:
 *
 *   /E (active-low enable) = E1M IO20 -- both U38 and U39 share /E
 *   S  (select)            = E1M IO21
 *
 * Per the 74LVC157 truth table:
 *   /E = 0, S = 0  ->  M.2 E-key SDIO routed to SoM
 *   /E = 0, S = 1  ->  microSD card slot routed to SoM
 *   /E = 1         ->  outputs Hi-Z (both buses isolated; safe-default)
 *
 * IMPORTANT: per the user-supplied wiring + this repo's
 * metadata/e1m_modules/aen/from-cc3501e.tsv, BOTH IO20 and IO21 are
 * proxied through the on-module CC3501E (GPIO26 and GPIO30 on the
 * CC3501E side).  Firmware drives the mux by dispatching
 * GPIO_WRITE commands to the CC3501E over the inter-chip SPI1
 * (see <alp/protocol/cc3501e.h>'s ALP_CC3501E_CMD_GPIO_WRITE),
 * NOT via Alif's GPIO peripheral. */
#define EVK_AEN_PIN_SDIO_MUX_EN                                                                    \
    ALP_E1M_GPIO_IO20 /**< /E -- drive low to enable the mux.  Routed through CC3501E. */
#define EVK_AEN_PIN_SDIO_MUX_SEL                                                                   \
    ALP_E1M_GPIO_IO21 /**< S  -- 0 = M.2 E SDIO, 1 = microSD.  Routed through CC3501E. */

typedef enum {
    EVK_AEN_SDIO_M2E_KEY = 0, /**< MUX_SEL.SDIO low. */
    EVK_AEN_SDIO_SDCARD  = 1, /**< MUX_SEL.SDIO high. */
} evk_aen_sdio_select_t;

/* I2S0 74LVC157 multiplexer (TAS2563 amplifier vs M.2 E-key I2S).
 *
 * Same shape as the SDIO mux: a 74LVC157 quad 2:1 picks which
 * device drives the SoM's single I2S0 bus.  Control pins:
 *
 *   /E (active-low enable) = E1M IO8   -- Alif side (P7.1)
 *   S  (select)            = E1M IO13  -- CC3501E side (GPIO13)
 *
 * IMPORTANT: the two control pins live on DIFFERENT chips on
 * this EVK -- I2S_EN is driven from Alif via alp_gpio_*, but
 * I2S_SELECT is on the CC3501E side and must be driven via
 * ALP_CC3501E_CMD_GPIO_WRITE on the inter-chip SPI1.  Apps that
 * switch the I2S routing need both code paths. */
#define EVK_AEN_PIN_I2S_MUX_EN                                                                     \
    ALP_E1M_GPIO_IO8 /**< /E -- Alif GPIO P7.1.  Drive low to enable mux. */
#define EVK_AEN_PIN_I2S_MUX_SEL                                                                    \
    ALP_E1M_GPIO_IO13 /**< S  -- CC3501E GPIO13.  Routed via CC3501E firmware. */

typedef enum {
    EVK_AEN_I2S_AMP     = 0, /**< I2S0 routed to the TAS2563 amplifiers. */
    EVK_AEN_I2S_M2E_KEY = 1, /**< I2S0 routed to the M.2 E-key slot. */
} evk_aen_i2s_select_t;

/* USB2 TMUXHS221 multiplexer (USB-A connector vs M.2 E-key USB).
 *
 * One TMUXHS221NKGR high-speed differential mux picks which USB
 * device drives the SoM's single USB2 port.  /OEN is hardwired
 * to GND on this EVK so the output is always enabled; only the
 * A/B select line is software-controlled:
 *
 *   USB2_SELECT = 0  ->  external USB-A connector (DA inputs)
 *   USB2_SELECT = 1  ->  M.2 E-key USB           (DB inputs)
 *
 * Drive the line via E1M IO11 -- which routes through the
 * on-module CC3501E (GPIO2 per from-cc3501e.tsv).  Firmware
 * drives the mux via ALP_CC3501E_CMD_GPIO_WRITE on the inter-chip
 * SPI1, NOT via Alif's GPIO peripheral. */
#define EVK_AEN_PIN_USB2_MUX_SEL                                                                   \
    ALP_E1M_GPIO_IO11 /**< 0 = USB connector, 1 = M.2 E-key USB.  Routed through CC3501E. */

typedef enum {
    EVK_AEN_USB2_CONNECTOR = 0, /**< External USB-A jack. */
    EVK_AEN_USB2_M2E_KEY   = 1, /**< M.2 E-key USB.        */
} evk_aen_usb2_select_t;

/** M.2 E-key UART wake signal.  Asserted by the M.2 module to
 *  request the host come out of low-power state.  Routes to E1M
 *  IO19, which lives on the CC3501E side (GPIO19 per
 *  from-cc3501e.tsv) -- firmware monitors it via
 *  ALP_CC3501E_CMD_GPIO_SET_INTERRUPT and propagates the wake
 *  event up to Alif. */
#define EVK_AEN_PIN_M2E_UART_WAKE ALP_E1M_GPIO_IO19

/* PCIe I2C mux + control-IO-expander cluster.
 *
 * The EVK splits E1M_I2C0 between the PCIe M-key and E-key slots
 * via a downstream mux gated by PCIE0_I2C.EN.  The select line
 * is driven from a SECOND TCAL9538 I/O expander (also on I2C0,
 * sitting BEFORE the mux so it stays addressable independent of
 * which PCIe slot the mux is currently routed to).  That second
 * expander also fans out the PCIe-side discrete control signals
 * (RST / WAKE / CLKREQ for both slots, plus M2E_ALERT).
 *
 * Three E1M-side signals control / observe the cluster:
 *   PCIE0_I2C.EN     -> E1M IO10  (drives I2C-mux enable)
 *   PCIE_IO_EXP.INT  -> E1M IO7   (interrupt from the PCIe expander)
 *   PCIE_IO_EXP.RST  -> E1M IO9   (reset for the PCIe expander)
 *
 * The PCIe expander is at I2C address 0x71 (A0=1, A1=0 per the
 * schematic strap).  Its alternate part TCA6408ARSVR uses
 * R112-only / R145-DNP per the schematic note. */
#define EVK_AEN_PIN_PCIE0_I2C_EN                                                                   \
    ALP_E1M_GPIO_IO10 /**< Drive high to enable I2C mux to PCIe slot. */
#define EVK_AEN_PIN_PCIE_IOEXP_INT ALP_E1M_GPIO_IO7 /**< INT input from the PCIe IO expander. */
#define EVK_AEN_PIN_PCIE_IOEXP_RST ALP_E1M_GPIO_IO9 /**< Reset output to the PCIe IO expander. */

/* PCIe LANE 2:1 multiplexer cluster (PI3DBS12212AXUAEX x4 +
 * SY75602BTWL-TR refclk buffer).
 *
 * Four PI3DBS12212 muxes split the SoM's PCIe lanes between the
 * M.2 M-key (x4) and E-key (x1) slots, plus a refclk buffer
 * forks the single PCIE0_REFCLK to both keys:
 *
 *   U42  TX0/TX1 mux  (E-key gets TX0 only; M-key gets TX0/TX1)
 *   U43  RX0/RX1 mux  (mirror for RX)
 *   U50  TX2/TX3 mux  (M-key only -- E-key has no lane 2/3)
 *   U51  RX2/RX3 mux  (M-key only)
 *   U44  SY75602      (REFCLK fork to both keys)
 *
 * The four lane muxes share two control pins:
 *   PCIe.MUX_PD   = E1M IO22  (1 = power-down, all outputs Hi-Z)
 *   PCIe.MUX_SEL  = E1M IO23  (selects between M-key and E-key)
 *
 * Apps that switch slots should:
 *   1. Power-down the active link first (PD = 1).
 *   2. Flip SEL.
 *   3. Wait the PCIe spec's PERST# de-assertion margin.
 *   4. Bring the muxes back up (PD = 0).
 *   5. Trigger PCIe re-enumeration on the host.
 *
 * U50/U51 take level-shifted versions (`PCIe.MUX_PD_L` and
 * `_SEL_L`) of the same signals -- U49 (a TXS0102) sits between
 * the SoM's IO22/IO23 and the PD_L/SEL_L nets.  Apps don't need
 * to think about that; the level shifter is transparent. */
#define EVK_AEN_PIN_PCIE_MUX_PD                                                                    \
    ALP_E1M_GPIO_IO22 /**< Drive HIGH to power down all four lane muxes.        */
#define EVK_AEN_PIN_PCIE_MUX_SEL                                                                   \
    ALP_E1M_GPIO_IO23 /**< Selects M-key vs E-key routing on the lane muxes. */

typedef enum {
    EVK_AEN_PCIE_E_KEY = 0, /**< Lanes 0 routed to PCIe E-key (Wi-Fi/BT modules). */
    EVK_AEN_PCIE_M_KEY = 1, /**< Lanes 0..3 routed to PCIe M-key (NVMe SSD).      */
} evk_aen_pcie_select_t;

/** PCIe IO expander pin layout (TCAL9538 #2 on I2C0 at 0x71). */
typedef enum {
    EVK_AEN_PCIE_IOEXP_I2C_SEL =
        0, /**< P0: PCIE0_I2C.SEL -- selects which slot the I2C mux routes to. */
    EVK_AEN_PCIE_IOEXP_M2E_ALERT      = 1, /**< P1: M.2 E-key alert input.        */
    EVK_AEN_PCIE_IOEXP_E_PCIE0_RST    = 2, /**< P2: E-key PCIe reset output.       */
    EVK_AEN_PCIE_IOEXP_E_PCIE0_WAKE   = 3, /**< P3: E-key PCIe wake input.         */
    EVK_AEN_PCIE_IOEXP_E_PCIE0_CLKREQ = 4, /**< P4: E-key PCIe clock-request input.*/
    EVK_AEN_PCIE_IOEXP_M_PCIE0_RST    = 5, /**< P5: M-key PCIe reset output.       */
    EVK_AEN_PCIE_IOEXP_M_PCIE0_WAKE   = 6, /**< P6: M-key PCIe wake input.         */
    EVK_AEN_PCIE_IOEXP_M_PCIE0_CLKREQ = 7, /**< P7: M-key PCIe clock-request input.*/
} evk_aen_pcie_ioexp_pin_t;

/* The rotary encoder's quadrature signals run through the SoC's
 * hardware quadrature counter on E1M's `ENC0_X` / `ENC0_Y` pads.
 * Use the E1M-standard `ALP_E1M_GPIO_ENC0_X` / `_Y` indices when
 * driving them as raw GPIOs. */

/* ================================================================== */
/* Pads repurposed by the EVK (off the E1M GPIO_IO numbering)         */
/*                                                                    */
/* The EVK's schematic ties a few non-IO pads to GPIO functions.      */
/* These can't ride the global `ALP_E1M_GPIO_IO*` namespace -- their  */
/* pad indices live past the standard 42-entry GPIO array.  The       */
/* carrier's `alp,pin-array` overlay extends the array with the       */
/* extra entries (indices 42..N) and the macros below map a name to  */
/* that overlay-defined index.                                        */
/*                                                                    */
/* The actual integer values are populated by the EVK's overlay (in  */
/* `alplabai/alp-zephyr-modules`); for studio-codegen consumers the  */
/* studio's pin allocator handles it.  Until that overlay lands, the */
/* macros below resolve to `EVK_AEN_PIN_OVERLAY_BASE + offset` and a */
/* hand-written-firmware author needs to verify the overlay declares */
/* matching extra entries.                                            */
/* ================================================================== */

/** Base index for EVK overlay-extended `alp,pin-array` entries.  Sits
 *  just past the 42 standard entries so it never collides. */
#define EVK_AEN_PIN_OVERLAY_BASE ALP_E1M_GPIO_COUNT

/** AUDIO_CLK pad (E1M Z2 / Alif P9_6) repurposed as the I/O
 *  expander INT line on this EVK.  When the audio path is in
 *  use the IO expander interrupt is unavailable; firmware should
 *  poll the expander instead. */
#define EVK_AEN_PIN_IO_EXP_INT (EVK_AEN_PIN_OVERLAY_BASE + 0u)

/** SPI0_CS1 pad (E1M N1 / Alif P3_6) repurposed as the I/O
 *  expander reset line.  When SPI0 is used with two chip-selects
 *  this pin can't double as IO_EXP_RST -- the EVK assumes SPI0
 *  is in single-CS mode at most. */
#define EVK_AEN_PIN_IO_EXP_RST (EVK_AEN_PIN_OVERLAY_BASE + 1u)

/** SPI0_MISO pad (E1M L1 / Alif P5_0) repurposed as the audio
 *  amplifier fault output (open-drain input from the amp). */
#define EVK_AEN_PIN_AMP_FAULT (EVK_AEN_PIN_OVERLAY_BASE + 2u)

/** SPI0_CS0 pad (E1M M1 / Alif P5_2) repurposed as the audio
 *  amplifier enable input (active-high). */
#define EVK_AEN_PIN_AMP_ENABLE (EVK_AEN_PIN_OVERLAY_BASE + 3u)

/** I2S1_SDI pad (E1M AH6 / Alif P13_4) repurposed as the
 *  mikroBUS click INT pin.  Was earlier (mis)documented as
 *  CTP_INT; the user has since clarified that CTP_INT is on
 *  SPI1_CS1 (see EVK_AEN_PIN_CTP_INT below) and I2S1_SDI is
 *  the mikroBUS INT line. */
#define EVK_AEN_PIN_MB_INT (EVK_AEN_PIN_OVERLAY_BASE + 4u)

/* CTP_RST: capacitive touch panel reset rides ONLY the TCAL9538
 * I/O expander on this EVK (pin P3 -- see EVK_AEN_IOEXP_CTP_RST
 * below).  An earlier draft of these notes had CTP_RST = SPI1_CS0
 * too; that was a mis-label.  SPI1_CS0 is the Arduino UNO
 * header's CK_CS (chip select); see "Arduino UNO header" below. */

/** SPI0_MOSI pad (E1M M2 / Alif P5_1) repurposed as Arduino
 *  CK_DIO4 (digital I/O 4 on the Arduino UNO header). */
#define EVK_AEN_PIN_CK_DIO4 (EVK_AEN_PIN_OVERLAY_BASE + 5u)

/** SPI0_SCLK pad (E1M N2) repurposed as Arduino CK_DIO3.
 *  NB: the Alif-side pad mapping for SPI0_SCLK is left blank in
 *  metadata/e1m_modules/aen/from-alif.tsv (user-supplied) and
 *  needs filling once the EVK schematic is cross-checked. */
#define EVK_AEN_PIN_CK_DIO3 (EVK_AEN_PIN_OVERLAY_BASE + 6u)

/** I2S1_WS pad (E1M AG7 / Alif P2_7) repurposed as Arduino CK_DIO2. */
#define EVK_AEN_PIN_CK_DIO2 (EVK_AEN_PIN_OVERLAY_BASE + 7u)

/** I2S1_SDO pad (E1M AG6 / Alif P13_5) repurposed as Arduino CK_DIO1. */
#define EVK_AEN_PIN_CK_DIO1 (EVK_AEN_PIN_OVERLAY_BASE + 8u)

/** I2S1_SCLK pad (E1M AH7 / Alif P2_6) repurposed as Arduino
 *  CK_RST (the Arduino UNO header's RESET signal -- shields can
 *  pulse it low to force a reboot). */
#define EVK_AEN_PIN_CK_RST (EVK_AEN_PIN_OVERLAY_BASE + 9u)

/** SPI1_CS1 pad (E1M AH8 -- CC3501E side, GPIO_15) repurposed as
 *  the capacitive touch panel interrupt input.  Routed through
 *  the on-module CC3501E -- firmware reads CTP touches by
 *  registering an interrupt callback on the CC3501E's GPIO_15
 *  via ALP_CC3501E_CMD_GPIO_SET_INTERRUPT. */
#define EVK_AEN_PIN_CTP_INT (EVK_AEN_PIN_OVERLAY_BASE + 10u)

/* I2S1 is fully consumed by the EVK -- all four I2S1 pads are
 * repurposed as GPIOs:
 *     I2S1_SDO  -> CK_DIO1
 *     I2S1_WS   -> CK_DIO2
 *     I2S1_SDI  -> CTP_INT
 *     I2S1_SCLK -> CK_RST
 * There is no peripheral I2S1 bus on this carrier; do not call
 * `alp_i2s_open(ALP_E1M_I2S1)` on the EVK -- it'll conflict with
 * the GPIO repurposes above.  The I2S0 path remains available
 * for audio. */

/* SPI0 is fully consumed by the EVK -- all five SPI0 pads
 * (MISO=AMP_FAULT, CS0=AMP_ENABLE, CS1=IO_EXP_RST, MOSI=CK_DIO4,
 * SCLK=CK_DIO3) are repurposed as GPIOs.  There is no peripheral
 * SPI0 bus available on this carrier; do NOT call
 * `alp_spi_open(ALP_E1M_SPI0)` on the EVK -- it'll work at the
 * wrapper level but conflict with the AMP / IOEXP / CK_DIO
 * routing above.  Use the CC3501E-mediated SPI surface for any
 * SPI device on the Arduino headers (see "Arduino UNO header"
 * section below). */

/* ================================================================== */
/* MIPI CSI camera multiplexer (PI3WVR626XEBEX)                       */
/* ================================================================== */

typedef enum {
    EVK_AEN_CAM_A = 0, /**< MIPI_CSI_<lane>_<P/N> -> A_MIPI_CSI_<lane>_<P/N> */
    EVK_AEN_CAM_B = 1, /**< MIPI_CSI_<lane>_<P/N> -> B_MIPI_CSI_<lane>_<P/N> */
} evk_aen_cam_select_t;

/* ================================================================== */
/* TCAL9538 I/O expander pin layout                                   */
/*                                                                    */
/* The TCAL9538 sits on E1M_I2C0 at 7-bit address 0x72 (A1=1, A0=0    */
/* per the EVK schematic).  Its 8 GPIO pins fan out to the LCD /      */
/* camera / capacitive-touch control lines and four sensor interrupt  */
/* inputs.  Apps drive them via the chips/tcal9538 driver:            */
/*                                                                    */
/*    tcal9538_t io_exp;                                              */
/*    tcal9538_init(&io_exp, i2c_bus, 0x72);                          */
/*    tcal9538_set_direction(&io_exp,                                 */
/*        BIT(EVK_AEN_IOEXP_LCD_PWR_EN) |                             */
/*        BIT(EVK_AEN_IOEXP_LCD_RST) |                                */
/*        BIT(EVK_AEN_IOEXP_CAM_EN)   |                               */
/*        BIT(EVK_AEN_IOEXP_CTP_RST),                                 */
/*        TCAL9538_DIR_OUTPUT);                                       */
/*    tcal9538_set(&io_exp, EVK_AEN_IOEXP_CAM_EN, true);              */
/* ================================================================== */

typedef enum {
    EVK_AEN_IOEXP_LCD_PWR_EN     = 0, /**< P0: LCD power enable.            */
    EVK_AEN_IOEXP_LCD_RST        = 1, /**< P1: LCD reset.                    */
    EVK_AEN_IOEXP_CAM_EN         = 2, /**< P2: Camera enable LDO.            */
    EVK_AEN_IOEXP_CTP_RST        = 3, /**< P3: Capacitive touch panel reset. */
    EVK_AEN_IOEXP_ICM42670_INT1  = 4, /**< P4: ICM-42670 INT1 input.         */
    EVK_AEN_IOEXP_ICM42670_INT2  = 5, /**< P5: ICM-42670 INT2 input.         */
    EVK_AEN_IOEXP_ICM42670_FSYNC = 6, /**< P6: ICM-42670 frame-sync input.   */
    EVK_AEN_IOEXP_BMP581_INT1    = 7, /**< P7: BMP581 INT1 input.            */
} evk_aen_ioexp_pin_t;

/* ================================================================== */
/* EVK bus assignments                                                */
/* ================================================================== */

/** Shared sensor + IO-expander + INA236 bus.  Maps to E1M's `I2C0`. */
#define EVK_AEN_I2C_BUS_SENSORS ALP_E1M_I2C0

/** Display + camera control I2C bus (touch panel, camera-side I2C
 *  configuration).  Maps to E1M's `I2C1` per the EVK schematic's
 *  DSI_CSI_I2C net. */
#define EVK_AEN_I2C_BUS_DSI_CSI ALP_E1M_I2C1

/** Arduino UNO header SPI bus.  Maps to E1M's `SPI1`, which on
 *  this EVK terminates on the on-module CC3501E (per
 *  metadata/e1m_modules/aen/from-cc3501e.tsv).  The Alif does NOT
 *  drive SPI1 directly here -- apps wanting to talk to an Arduino
 *  shield's SPI device dispatch through the CC3501E firmware.
 *  CK_CS = E1M_SPI1_CS0 is the chip-select line.
 *
 *  M.2 (Key M and Key E) on the EVK carrier uses PCIe + SDIO,
 *  not SPI; the previous `EVK_AEN_SPI_BUS_M2_KEYM` macro was a
 *  guess and has been removed. */
#define EVK_AEN_SPI_BUS_ARDUINO ALP_E1M_SPI1

/** Arduino UNO header I2C bus.  Maps to E1M's `I3C0` -- the
 *  Arduino's SCL/SDA on this EVK ride the I3C bus (which is
 *  backwards-compatible with classic I2C).  Apps that only need
 *  I2C semantics use it as such; I3C-aware apps get the
 *  higher-rate path. */
#define EVK_AEN_I2C_BUS_ARDUINO ALP_E1M_I3C0

/** Console UART exposed on the JTAG/SWD-side debug header.  Maps to `UART0`. */
#define EVK_AEN_UART_PORT_DEBUG ALP_E1M_UART0

/** Arduino UNO header UART (D0/D1).  Maps to E1M's `UART1`.
 *  Net-naming note: CK_RXD = E1M_UART1_TX, CK_TXD = E1M_UART1_RX
 *  (the Arduino-shield "RX" pin reads the host's TX, and vice
 *  versa). */
#define EVK_AEN_UART_PORT_ARDUINO ALP_E1M_UART1

/* ================================================================== */
/* Arduino UNO header pin mappings (full)                             */
/*                                                                    */
/* The EVK exposes a standard Arduino UNO-form-factor header.  Each   */
/* CK_<name> / ARD_<name> macro below maps a header pin to the        */
/* underlying E1M-standard ID -- channel ID for PWM / ADC / bus       */
/* instances; OVERLAY_BASE + N for the digital I/Os that ride         */
/* repurposed peripheral pads.                                        */
/* ================================================================== */

/* PWM (header pins driven by a PWM channel). */
#define EVK_AEN_ARD_PWM1 ALP_E1M_PWM1 /**< CK_PWM1 = E1M PWM1. */
#define EVK_AEN_ARD_PWM2 ALP_E1M_PWM4 /**< CK_PWM2 = E1M PWM4. */
#define EVK_AEN_ARD_PWM3 ALP_E1M_PWM5 /**< CK_PWM3 = E1M PWM5. */
#define EVK_AEN_ARD_PWM4 ALP_E1M_PWM2 /**< CK_PWM4 = E1M PWM2. */

/* Digital I/O (header pins riding repurposed peripheral pads --
 * the OVERLAY_BASE + N indices defined above). */
#define EVK_AEN_ARD_DIO1 EVK_AEN_PIN_CK_DIO1 /**< CK_DIO1 = I2S1_SDO. */
#define EVK_AEN_ARD_DIO2 EVK_AEN_PIN_CK_DIO2 /**< CK_DIO2 = I2S1_WS. */
#define EVK_AEN_ARD_DIO3 EVK_AEN_PIN_CK_DIO3 /**< CK_DIO3 = SPI0_SCLK. */
#define EVK_AEN_ARD_DIO4 EVK_AEN_PIN_CK_DIO4 /**< CK_DIO4 = SPI0_MOSI. */
#define EVK_AEN_ARD_RST EVK_AEN_PIN_CK_RST   /**< CK_RST  = I2S1_SCLK. */

/* Analog (header pins on E1M ADC channels). */
#define EVK_AEN_ARD_A0 ALP_E1M_ADC0 /**< ARD.A0 = E1M ANA_S0. */
#define EVK_AEN_ARD_A1 ALP_E1M_ADC1 /**< ARD.A1 = E1M ANA_S1. */
#define EVK_AEN_ARD_A2 ALP_E1M_ADC2 /**< ARD.A2 = E1M ANA_S2. */
#define EVK_AEN_ARD_A3 ALP_E1M_ADC3 /**< ARD.A3 = E1M ANA_S3. */
#define EVK_AEN_ARD_A4 ALP_E1M_ADC4 /**< ARD.A4 = E1M ANA_S4. */
#define EVK_AEN_ARD_A5 ALP_E1M_ADC5 /**< ARD.A5 = E1M ANA_S5. */

/* ================================================================== */
/* mikroBUS click header                                              */
/*                                                                    */
/* The EVK exposes a standard mikroBUS click header next to the       */
/* Arduino UNO header.  Most pins are SHARED with the Arduino         */
/* header -- only the mikroBUS-only signals (CK_PWM0, CK_INT,         */
/* CK_ANA) get fresh macros here.  See the inline note below for      */
/* the shared-pin map.                                                */
/* ================================================================== */

/** mikroBUS PWM pin.  Maps to E1M_PWM6. */
#define EVK_AEN_MB_PWM ALP_E1M_PWM6

/** mikroBUS INT pin.  Maps to E1M I2S1_SDI -- the dedicated
 *  mikroBUS interrupt line on this EVK.  No longer shared with
 *  CTP_INT (the user clarified CTP_INT lives on SPI1_CS1
 *  instead -- see EVK_AEN_PIN_CTP_INT above). */
#define EVK_AEN_MB_INT EVK_AEN_PIN_MB_INT

/** mikroBUS ANA pin.  Shared with Arduino's ARD.A0 -- both pins
 *  route to E1M ANA_S0 (ALP_E1M_ADC0).  Apps that mount only one
 *  of {Arduino shield, mikroBUS click} get unambiguous use of
 *  ADC0; mounting both forces a contention. */
#define EVK_AEN_MB_ANA ALP_E1M_ADC0

/* mikroBUS shared with Arduino -- use the ARD_* / EVK_AEN_* macros:
 *   CK_RST  -> EVK_AEN_ARD_RST              (I2S1_SCLK)
 *   CK_CS   -> EVK_AEN_SPI_BUS_ARDUINO + cs (SPI1_CS0)
 *   CK_SCK  -> EVK_AEN_SPI_BUS_ARDUINO clock (SPI1_SCLK)
 *   CK_MISO -> EVK_AEN_SPI_BUS_ARDUINO MISO (SPI1_MISO)
 *   CK_MOSI -> EVK_AEN_SPI_BUS_ARDUINO MOSI (SPI1_MOSI)
 *   CK_TXD  -> EVK_AEN_UART_PORT_ARDUINO TX
 *   CK_RXD  -> EVK_AEN_UART_PORT_ARDUINO RX
 *   CK_SCL  -> EVK_AEN_I2C_BUS_ARDUINO  SCL (I3C_SCL)
 *   CK_SDA  -> EVK_AEN_I2C_BUS_ARDUINO  SDA (I3C_SDA)
 *
 * Sharing means a mikroBUS click + Arduino shield cannot both
 * drive any of the shared lines simultaneously -- the EVK trusts
 * the user to mount only one of the two physical headers. */

/* ================================================================== */
/* On-board sensor 7-bit I2C addresses                                */
/*                                                                    */
/* All on E1M_I2C0 (the sensor bus).  Strap values per the EVK        */
/* schematic UG-E1M-001 + user-supplied confirmation:                 */
/*   - ICM-42670-P  AD0/SD0 = high     -> 0x69                        */
/*   - BMI323       SDO_MISO_ADR = low -> 0x68 (no collision)         */
/*   - BMP581       SDO = high         -> 0x47                        */
/*   - TCAL9538     A1=1, A0=0         -> 0x72                        */
/* ================================================================== */

#define EVK_AEN_I2C_ADDR_ICM42670 0x69u /**< U12 IMU (AD0=1). */
#define EVK_AEN_I2C_ADDR_BMI323 0x68u   /**< U13 IMU (SDO=0).  No collision with ICM. */
#define EVK_AEN_I2C_ADDR_BMP581 0x47u   /**< U14 barometer (SDO=1). */
/* The EVK populates TWO TCAL9538 I/O expanders, both on E1M_I2C0
 * but at different strap-selected addresses:
 *   - The "main" expander handles LCD / camera / capacitive-touch
 *     control + four sensor interrupt inputs (see
 *     evk_aen_ioexp_pin_t).  Strap A1=1, A0=0 -> 0x72.
 *   - The "PCIe" expander handles the I2C-mux SEL + PCIe slot
 *     RST/WAKE/CLKREQ signals + M2E_ALERT (see
 *     evk_aen_pcie_ioexp_pin_t above).  Strap A0=1, A1=0 -> 0x71. */
#define EVK_AEN_I2C_ADDR_TCAL9538_MAIN 0x72u /**< U35 main I/O expander. */
#define EVK_AEN_I2C_ADDR_TCAL9538_PCIE 0x71u /**< U37 PCIe I/O expander. */
/** Backward-compat alias -- legacy name for the main expander. */
#define EVK_AEN_I2C_ADDR_TCAL9538 EVK_AEN_I2C_ADDR_TCAL9538_MAIN

/* Two TAS2563RPP smart-amp ICs share the same I2C0 bus.  AD0
 * strap selects address per TAS2563 datasheet table 7-3:
 *   AD0 = 10k to GND  -> 0x4D  (one channel)
 *   AD0 = 10k to VDD  -> 0x4E  (the other)
 * Both also respond to the global broadcast 0x48 (write-only;
 * useful for synchronised setup, never for register reads). */
#define EVK_AEN_I2C_ADDR_TAS2563_LOW 0x4Du
#define EVK_AEN_I2C_ADDR_TAS2563_HIGH 0x4Eu
#define EVK_AEN_I2C_ADDR_TAS2563_BCAST 0x48u

/* Six INA236 high-side current-shunt monitors -- one per power
 * rail -- on I2C0.  TI's INA236A variant occupies 0x40..0x43 and
 * the INA236B variant occupies 0x44..0x47, so all six fit on one
 * bus despite each variant only having 2 strap bits.
 *
 * EVK ref-des per the user-confirmed schematic:
 *   U21  INA236A  3V3 rail     A0 = GND  -> 0x40
 *   U31  INA236A  1V8 rail     A0 = V+   -> 0x41
 *   U33  INA236A  VIO rail     A0 = SDA  -> 0x42
 *   U32  INA236B  +V_CAM0      A0 = GND  -> 0x44
 *   U34  INA236B  +V_CAM1      A0 = V+   -> 0x45
 *   U30  INA236B  5V  rail     A0 = SDA  -> 0x46
 */
#define EVK_AEN_I2C_ADDR_INA236_3V3 0x40u   /**< U21 INA236A. */
#define EVK_AEN_I2C_ADDR_INA236_1V8 0x41u   /**< U31 INA236A. */
#define EVK_AEN_I2C_ADDR_INA236_VIO 0x42u   /**< U33 INA236A. */
#define EVK_AEN_I2C_ADDR_INA236_VCAM0 0x44u /**< U32 INA236B. */
#define EVK_AEN_I2C_ADDR_INA236_VCAM1 0x45u /**< U34 INA236B. */
#define EVK_AEN_I2C_ADDR_INA236_5V 0x46u    /**< U30 INA236B. */

/* ================================================================== */
/* On-board audio I/O                                                 */
/*                                                                    */
/* Audio in (PDM):                                                    */
/*   Four STMicro MP34DT05TR-A digital MEMS microphones, two pairs:   */
/*     U19 LEFT  + U20 RIGHT  on E1M PDM0  (CK = PDM_C0, D = PDM_D0)  */
/*     U25 LEFT  + U26 RIGHT  on E1M PDM1  (CK = PDM_C1, D = PDM_D1)  */
/*   Each LEFT/RIGHT pair shares one PDM data line -- LEFT mics tie  */
/*   their LR pin high (data on rising clock edge), RIGHT mics tie   */
/*   LR low (data on falling edge), and the single data line carries */
/*   both interleaved.  Apps open the streams via alp_audio_in_open  */
/*   with peripheral_id = 0 (PDM0) or 1 (PDM1), channels = 2 (stereo). */
/*                                                                    */
/* Audio out (I2S):                                                   */
/*   Two TAS2563RPP smart-amp ICs (one channel each, addresses 0x4D  */
/*   and 0x4E) sit on E1M I2S0.  Audio flows host -> amp on          */
/*   I2S0_SDO; the amp reports IV-sense data back on I2S0_SDI.       */
/*   Use chips/tas2563 to bring them up; AMP.ENABLE / AMP.FAULT are  */
/*   on EVK_AEN_PIN_AMP_ENABLE / EVK_AEN_PIN_AMP_FAULT respectively. */
/*                                                                    */
/*   I2S0 itself can be re-routed away from the amps to the M.2      */
/*   E-key slot via the I2S 74LVC157 mux -- see                      */
/*   EVK_AEN_PIN_I2S_MUX_EN + EVK_AEN_PIN_I2S_MUX_SEL above.         */
/* ================================================================== */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BOARDS_ALP_E1M_EVK_AEN_H */
