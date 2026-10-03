#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# Opt-in USB device (gadget) setup for the E1M-X EVK USB 2.0 port:
# one composite gadget with an Ethernet function (ECM) and a serial
# function (ACM). Usage: alp-usb-gadget.sh start|stop
#
# Needs usb-gadget.cfg in the kernel (configfs + f_ecm + f_acm) and the
# port in the peripheral role -- see docs/v2n-usb-otg.md. 1d6b:0104 is the
# Linux Foundation "Multifunction Composite Gadget" id intended for
# configfs gadgets; replace it with your own id for a shipped product.
set -eu

G=/sys/kernel/config/usb_gadget/alp
CFS=/sys/kernel/config

case "${1:-}" in
start)
	mountpoint -q "$CFS" || mount -t configfs none "$CFS"
	[ -d "$G" ] && { echo "alp-usb-gadget: already set up" >&2; exit 0; }
	UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
	[ -n "$UDC" ] || { echo "alp-usb-gadget: no UDC (port not in device role?)" >&2; exit 1; }
	mkdir -p "$G"
	echo 0x1d6b > "$G/idVendor"
	echo 0x0104 > "$G/idProduct"
	mkdir -p "$G/strings/0x409"
	echo "Alp Lab AB"   > "$G/strings/0x409/manufacturer"
	echo "E1M-X EVK USB gadget" > "$G/strings/0x409/product"
	mkdir -p "$G/configs/c.1/strings/0x409"
	echo "ECM+ACM" > "$G/configs/c.1/strings/0x409/configuration"
	mkdir -p "$G/functions/ecm.usb0" "$G/functions/acm.GS0"
	ln -s "$G/functions/ecm.usb0" "$G/configs/c.1/"
	ln -s "$G/functions/acm.GS0" "$G/configs/c.1/"
	echo "$UDC" > "$G/UDC"
	;;
stop)
	[ -d "$G" ] || exit 0
	echo "" > "$G/UDC"
	rm -f "$G/configs/c.1/ecm.usb0" "$G/configs/c.1/acm.GS0"
	rmdir "$G/configs/c.1/strings/0x409" "$G/configs/c.1" \
		"$G/functions/ecm.usb0" "$G/functions/acm.GS0" \
		"$G/strings/0x409" "$G"
	;;
*)
	echo "usage: $0 start|stop" >&2
	exit 2
	;;
esac
