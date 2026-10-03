### Added — E1M-X EVK USB 2.0 OTG gadget mode and a Bluetooth HFP/SCO routing note (#2660)

The USB 2.0 port keeps `dr_mode = "otg"`. Device mode is expected to work
but is bench-unverified: VBUS sense is not wired, so attach/detach is not
detected automatically, and the role is switched by hand. The dtsi comments
and errata E3 no longer claim an always-on VBUS. Setting
`ALP_ENABLE_USB_GADGET = "1"` merges the new `usb-gadget.cfg` kernel
fragment and installs the opt-in `alp-usb-gadget` recipe (NCM + ACM, so
Windows 10/11 and macOS need no driver); default images are unchanged.
`docs/v2n-bt-hfp-audio.md` records the BT PCM/I2S pad routing
and the missing pin-function data; no device tree is added yet. Bench-unverified.
