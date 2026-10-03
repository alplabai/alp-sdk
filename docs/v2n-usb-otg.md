# E1M-X EVK USB 2.0 OTG (V2N / V2M)

Status: device mode implemented, **bench-unverified**. Host mode and role
switching are limited by the carrier, stated below.

## What the hardware allows

- The USB 2.0 receptacle is a Type-C **sink** (CC pull-downs, no VBUS source).
  The carrier cannot power a device plugged into it.
- The SoC VBUS-sense input is left open and the carrier's data-path mux
  select rides on E1M IO24, which the V2N family cannot drive (14 pads
  reach only DX-M1 GPIOs). No ID or VBUS-detect line reaches software.

| Role | Works | How |
|---|---|---|
| Device (gadget) | yes | Plug into a PC; the PC supplies VBUS |
| Host | only with external VBUS | A self-powered hub; the port will not source 5 V |
| Automatic role switch | no | No ID/VBUS-detect line; switch by hand |

## Device tree

`e1m-x-evk.dtsi` sets `dr_mode = "otg"` on `&hsusb`, `&ehci0` and `&ohci0`.
Over-current is suppressed at the controllers (`spurious-oc`, errata E3).
Role switching is manual through the USB 2.0 PHY's sysfs `role` attribute
(`host` / `peripheral`; locate it under the PHY platform device in
`/sys/devices/platform/`, the exact path is bench-to-confirm).

## Kernel config and gadget script

- `usb-gadget.cfg` (merged on the V2N-family machines): USB gadget, Renesas
  USBHS device controller, configfs, ECM, NCM, ACM.
- `alp-usb-gadget` recipe: `alp-usb-gadget.sh start|stop` builds one
  composite gadget (ECM network + ACM serial). Opt-in: set
  `ALP_USB_GADGET = "1"` in `local.conf`. The systemd unit is installed but
  not enabled at boot. The `1d6b:0104` id is the generic Linux Foundation
  composite id; use your own for a shipped product.

## HIL spec (device mode from a PC) - not yet run

1. Flash an image built with `ALP_USB_GADGET = "1"`; boot; log in on the console.
2. `ls /sys/class/udc` - expect one controller. If empty, set the PHY `role`
   to `peripheral` (see above) and check `dmesg | grep -i usb`.
3. `alp-usb-gadget.sh start`.
4. Connect the USB 2.0 receptacle to a PC with a data cable.
5. On the PC, expect a new network interface (ECM) and a serial port (ACM:
   `/dev/ttyACM*` on Linux, a COM port on Windows).
6. Give both ends an address (`ip addr add 192.168.7.1/24 dev usb0` on the
   board, `192.168.7.2/24` on the PC) and `ping` both ways.
7. On the board `getty`/`cat` on `/dev/ttyGS0`, on the PC open the ACM port;
   type in both directions.
8. `alp-usb-gadget.sh stop` - the PC drops both devices.
9. Host check: set role `host`, connect a self-powered hub with a stick,
   expect enumeration in `lsusb`. Record the result in errata E3.
