### Added — E1M-X EVK USB 2.0 OTG gadget mode and a Bluetooth HFP/SCO routing note (#2660)

The USB 2.0 port keeps `dr_mode = "otg"` but the carrier has no VBUS source
and no ID/VBUS-detect line, so device mode works, host mode needs external
VBUS, and the role is switched by hand; the dtsi comments and errata E3 no
longer claim an always-on VBUS. A new `usb-gadget.cfg` kernel fragment and an
opt-in `alp-usb-gadget` recipe (`ALP_USB_GADGET = "1"`) provide an ECM + ACM
gadget. `docs/v2n-bt-hfp-audio.md` records the confirmed BT PCM/I2S routing
and the missing pin-function data; no device tree is added yet. Bench-unverified.
