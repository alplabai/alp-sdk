# E1M-X EVK USB 2.0 OTG (V2N / V2M)

Status: device mode is **expected to work, bench-unverified**. VBUS sense is
not wired, so attach/detach is not detected automatically (the USBHS
controller normally connects its D+ pull-up only after seeing VBUS-valid;
whether the PHY forces it in the peripheral role is unverified).

## What the hardware allows

- Per the private carrier netlist (detail not reproduced here, not
  bench-confirmed): the USB 2.0 receptacle has no VBUS source and the SoC
  VBUS-sense input is not wired. No ID or VBUS-detect line reaches software.
- The USB path-mux select is E1M IO24 (`XEVK_PIN_USB_MUX_SEL`,
  `metadata/boards/e1m-x-evk.yaml`). On V2M it is driven by the DX-M1; on
  V2N nothing drives it (`metadata/e1m_modules/E1M-V2M101.yaml`,
  `E1M-V2N101.yaml`), so the mux position depends on the carrier default,
  which is not in this repo. That the receptacle is routed to the SoC at
  all is therefore a HIL step below, not an assumption.

| Role | Status | How |
|---|---|---|
| Device (gadget) | expected, unverified | Plug into a PC; the PC supplies VBUS |
| Host | needs external VBUS | A self-powered hub |
| Automatic role switch | no | No ID/VBUS-detect line; switch by hand |

## Device tree

`e1m-x-evk.dtsi` sets `dr_mode = "otg"` on `&hsusb`, `&ehci0` and `&ohci0`.
Over-current is suppressed at the controllers (`spurious-oc`, errata E3).
Role switching is manual through the USB 2.0 PHY's sysfs `role` attribute
(`host` / `peripheral`; locate it under the PHY platform device in
`/sys/devices/platform/`, the exact path is bench-to-confirm).

## Kernel config and gadget script

Everything here is opt-in: set `ALP_ENABLE_USB_GADGET = "1"` in `local.conf`.
Default images are unchanged (the Renesas USBHS driver is not built in, so
the USB 2.0 host path stays as errata E3 verified it).

- `usb-gadget.cfg` (merged only with the opt-in): USB gadget, Renesas USBHS
  device controller, configfs, NCM, ACM.
- `alp-usb-gadget` recipe: `alp-usb-gadget.sh start|stop` builds one
  composite gadget (NCM network + ACM serial, IAD composite). NCM and ACM
  bind to in-box drivers on Linux, macOS and Windows 10/11. The systemd unit
  is installed but not enabled at boot. The `1d6b:0104` id is the generic
  Linux Foundation composite id; use your own for a shipped product.

## HIL spec (device mode from a PC) - not yet run

1. Flash an image built with `ALP_ENABLE_USB_GADGET = "1"`; boot; log in on
   the console.
2. `ls /sys/class/udc` - expect one controller. If empty the UDC did not
   probe: check `do_kernel_configcheck` for the USBHS symbols,
   `dmesg | grep -i usbhs` and the `&hsusb` binding. (The role does not
   affect whether the UDC registers.)
3. `alp-usb-gadget.sh start`.
4. Connect the USB 2.0 receptacle to a PC with a data cable (this also
   checks that the receptacle is routed to the SoC given the IO24 mux).
5. Check `cat /sys/class/udc/*/state` moves past `not attached` and the PC
   logs an enumeration event. If it stays `not attached` with the UDC
   present, set the PHY `role` to `peripheral` (see above) and retry;
   record whether VBUS-valid had to be forced.
6. On the PC, expect a new network interface (NCM) and a serial port (ACM:
   `/dev/ttyACM*` on Linux, a COM port on Windows, no driver install).
7. Give both ends an address (`ip addr add 192.168.7.1/24 dev usb0` on the
   board, `192.168.7.2/24` on the PC) and `ping` both ways.
8. On the board `getty`/`cat` on `/dev/ttyGS0`, on the PC open the ACM port;
   type in both directions.
9. `alp-usb-gadget.sh stop` - the PC drops both devices.
10. Host check: set role `host`, connect a self-powered hub with a stick,
    expect enumeration in `lsusb`. Record the result in errata E3.
11. Regression: on an image built with the opt-in OFF, USB 2.0 host behaviour
    is unchanged (no OC lines at boot, per errata E3).
