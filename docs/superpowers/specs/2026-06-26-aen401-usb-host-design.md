> **SUPERSEDED (2026-06-26)** by `2026-06-26-aen401-xhci-usb-host-design.md` — the IP is xHCI (DWC3-family), not DWC2. The DWC2 host-channel approach here is wrong; see the xHCI spec.

# E1M-AEN401 M55/Zephyr USB host (on-SoC DWC2)

**Date:** 2026-06-26
**Branch:** `feat/aen401-usb-host` (off `dev`)
**Driver:** a customer wants USB host on the Alif **Ensemble E4** (E1M-AEN401), which
is **M55-only (no Cortex-A32)** — so there is no Linux USB-host fallback; Zephyr is
the only runtime, and Zephyr today has **no USB-host (`uhc`) driver for the Alif
DWC2 controller**. External host controllers (e.g. MAX3421E) are ruled out by the
customer, so the on-SoC DWC2 in host mode is the only path.

## Goal

Stand up the M55/Zephyr USB host stack for E1M-AEN401: a DWC2-host `uhc` driver in
the alp-sdk Zephyr module, the `alp_usb_host_*` backend wired to Zephyr's `usbh`
stack, and a mass-storage host example — all building no-board for the AEN401 M55,
with the live enumeration left to the bench.

## Non-goals

- Live USB enumeration / device class operation on silicon (board-gated).
- USB **device** role (already supported via the existing `udc`/device path) — only
  the host role is added.
- E6/E8 USB-host validation — the driver is written SoC-IP-generic (it will serve
  them later), but AEN401 is the validation target here.
- Full AEN401 SoM bring-up beyond what the USB host example needs to build.

## Grounding (authoritative)

| Fact | Source |
|---|---|
| E4 is M55-only: `m55_hp` + `m55_he` (no A-core) | `metadata/socs/alif/ensemble/e4.json` `cores[]`; `metadata/e1m_modules/E1M-AEN401.yaml` |
| E4's USB controller is **unconfirmed in metadata** — `e4.json` `peripherals: {}` is empty | `e4.json` (a known ingest gap, status notwithstanding) |
| Sibling **E3 has `usb_2`** (and E5/E7/E8 too) — the Ensemble line shares the Synopsys DWC2 USB2 IP → E4 near-certainly has it | `metadata/socs/alif/ensemble/e3.json` `peripherals.usb_2` |
| alp host API exists, NOSUPPORT-gated: `alp_usb_host_open/enable/disable/close` | `include/alp/usb.h` |
| Backend host ops exist as NOSUPPORT stubs: `z_host_open/enable/disable/close` | `src/backends/usb/zephyr_drv.c`, `src/backends/usb/usb_ops.h` |
| Zephyr 4.4 host stack API: `usbh_init/enable/disable/shutdown(struct usbh_context *)`, `USBH_CONTROLLER_DEFINE` | `~/zephyrproject/zephyr/include/zephyr/usb/usbh.h` |
| `uhc_api` driver ops: `lock/unlock/init/enable/disable/shutdown/bus_reset/sof_enable/bus_suspend/bus_resume/ep_enqueue/ep_dequeue` | `~/zephyrproject/zephyr/include/zephyr/drivers/usb/uhc.h` |
| Zephyr has `uhc` drivers for max3421e/mcux/virtual only — **no DWC2-host** | `~/zephyrproject/zephyr/drivers/usb/uhc/` |
| alp-sdk is a Zephyr module that ships drivers under `zephyr/drivers/` | `zephyr/module.yml`, `zephyr/drivers/` |
| No Zephyr board exists for AEN401 yet | `zephyr/boards/alp/` (no `e1m_aen401*`) |

## Components

### Component 0 — E4 USB grounding (feasibility gate)

Confirm E4 carries the DWC2 USB2 controller and populate
`metadata/socs/alif/ensemble/e4.json` `peripherals.usb_2` (and any USB topology the
generated board needs) from the **E4 datasheet/HWRM** (`e4.json` carries the
`datasheet`/`hwrm` refs). If the datasheet shows E4 has **no** USB controller, STOP
and escalate — the request is infeasible on E4 (external ruled out). The E3 sibling
makes "E4 has DWC2 USB2" the strong expectation; this step makes it authoritative,
not inferred.

### Component 1 — DWC2-host `uhc` driver (skeleton, SoC-IP-generic)

`zephyr/drivers/usb/uhc/uhc_dwc2_alif.{c,h}` + `Kconfig` + `CMakeLists.txt` (wired
into `zephyr/drivers/usb/` via the module) + DT binding
`zephyr/dts/bindings/usb/alif,dwc2-uhc.yaml`. Implements the `uhc_api` op table and
the DWC2 host register map; registers a controller via `USBH_CONTROLLER_DEFINE`.

**Structurally complete but UNVALIDATED** — USB host bring-up is timing/HW-dependent
("a clean link ≠ runs"). Every op whose behavior can't be verified no-board carries
a `TODO(aen401-bench)` marker; the op table, register defines, ISR skeleton, and DT
binding are real. Modeled on the existing `udc/dwc2` device driver's register/IP
conventions (same Synopsys core) and Zephyr's `uhc_max3421e`/`uhc_mcux` op-table
shape.

### Component 2 — alp backend `usbh` wiring (SoM-agnostic)

`src/backends/usb/zephyr_drv.c`: replace the `z_host_*` NOSUPPORT stubs with calls
into Zephyr's host stack — `z_host_open → usbh_init`, `z_host_enable → usbh_enable`,
`z_host_disable → usbh_disable`, `z_host_close → usbh_shutdown` — holding a
`struct usbh_context` in `alp_usb_host_state_t`. Compiled behind
`CONFIG_USB_HOST_STACK` (so non-host builds keep the NOSUPPORT path). Capabilities
report host-capable when the stack is present.

### Component 3 — AEN401 M55 board + mass-storage host example

- **Board:** generate the AEN401 M55-HP Zephyr board (`alp_e1m_aen401_m55_hp`, from
  the metadata once Component 0 populates USB), with the USB controller node bound
  to `alif,dwc2-uhc`. Boards are generated, not hand-authored
  (`scripts/alp_project.py --emit zephyr-board`); if generation surfaces AEN401 gaps
  beyond USB, scope them minimally or flag as a dependency.
- **Example:** `examples/peripheral-io/usb-host-storage/` — `alp_usb_host_open` →
  `alp_usb_host_enable` → enumerate → mount a USB mass-storage device + list its root
  (the customer's USB-stick use case). HID is documented as a sibling, not built.

## Validation (no-board)

- The DWC2-host driver, the backend `usbh` wiring, and the example all **compile**
  for the AEN401 M55 board (`west build -b alp_e1m_aen401_m55_hp ...`) with
  `CONFIG_USB_HOST_STACK`/`CONFIG_UHC_DRIVER` on — link-clean, `uhc_api` table
  complete.
- The alp backend layer also builds on the existing host targets (native/the
  unit-test harness) behind the host-stack guard, so `z_host_*` wiring is exercised
  at compile time without the controller.
- Metadata/schema + clang-format-22 + the orchestrate/board-gen gates pass.

## Board-gated (bench, `TODO(aen401-bench)`)

- E4-USB datasheet confirmation if the metadata ingest can't settle it no-board.
- The DWC2-host driver's actual bring-up: bus reset, enumeration, control/bulk
  transfers, the ISR/endpoint paths.
- Live `alp_usb_host_*` enumeration + mounting a real USB stick on the AEN401 M55.

## Risks

- **E4-USB existence (Component 0).** If E4 genuinely lacks an on-SoC USB controller,
  the request is infeasible (external ruled out) → escalate, do not fake. Strong
  prior (E3 sibling) that it exists.
- **Unvalidated driver.** A USB-host controller driver written without hardware reads
  "done" but isn't — the skeleton + register map are real, bring-up is bench work.
  Mark `TODO(aen401-bench)` honestly; do not claim runtime correctness.
- **AEN401 board generation.** AEN401 is a not-yet-ported SoM; generating its M55
  board may surface metadata gaps (mailbox `TBD`, peripherals) beyond USB. Keep the
  board scope minimal (enough to build the USB example); flag a larger AEN401
  bring-up as a separate dependency if it balloons.
- **Zephyr version skew.** The `usbh`/`uhc` APIs are evolving; pin to the version the
  alp-sdk Zephyr module targets (4.4.x in the validated workspace) and note it.

## Constraints

- Never invent register offsets/addresses — transcribe DWC2 host registers from the
  Synopsys DWC2 / Alif HWRM (or mirror the in-tree `udc/dwc2` device driver's
  defines for the shared core), else `TODO(aen401-bench)`.
- New C obeys clang-format-22 **except** files under `zephyr/**` (excluded from the
  gate — the new `uhc_dwc2_alif.c` lives there and follows Zephyr driver style).
- "Alp Lab" spelling; no `Co-Authored-By: Claude` footer. `py -3.14` for Python.
- No binaries in git. Branch `feat/aen401-usb-host` → PR to `dev`.
