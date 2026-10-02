# SPDX-License-Identifier: Apache-2.0
"""USB transport and wire encoding for the probe's CMSIS-DAP v2 vendor commands.

Encoders/decoders follow the host reference implementation in the public
alplabai/rp2040-debugprobe-firmware repo (tools/alp_probe.py, MIT,
Copyright (c) 2026 Alp Lab AB); copied here so this package is self-contained.
Little-endian; the first response byte echoes the command ID.
"""
import struct


class ProbeError(Exception):
    """A probe problem the user can act on (message is shown verbatim)."""


# Placeholder VID/PID of the probe (see the firmware repo README).
VID, PID = 0x2E8A, 0x000C
CMD_INFO, CMD_I2C, CMD_I2C_CONFIG = 0x80, 0x81, 0x82
CMD_PIN_SET, CMD_PIN_GET, CMD_PIN_INFO = 0x83, 0x84, 0x85
CMD_STREAM_CONFIG, CMD_START, CMD_STOP, CMD_READ = 0x86, 0x87, 0x88, 0x89
NO_MARKER = 0xFF
STATUS = ["ok", "nack", "timeout", "bad length or address", "no I2C on this probe",
          "busy (stream running)", "stream not configured"]


def status_text(st):
    return STATUS[st] if st < len(STATUS) else f"status {st}"


def dec_info(r):
    assert r[0] == CMD_INFO, r
    return dict(version=r[1], i2c=bool(r[2] & 1), gpio=bool(r[2] & 2),
                stream=bool(r[2] & 4), npins=r[3], i2c_max=r[4])


def enc_i2c(addr, wdata=b"", rlen=0, restart=False):
    return bytes([CMD_I2C, addr, 1 if restart else 0, len(wdata), rlen]) + wdata


def enc_i2c_config(hz):
    return bytes([CMD_I2C_CONFIG]) + struct.pack("<I", hz)


def dec_pin_info(r):
    assert r[0] == CMD_PIN_INFO, r
    return r[1], bytes(r[4:4 + r[3]]).decode("ascii")  # status, name


def enc_stream_config(period_us, marker, chans):
    """chans: list of (addr7, reg, len); marker NO_MARKER = none."""
    out = bytes([CMD_STREAM_CONFIG]) + struct.pack("<IBB", period_us, marker, len(chans))
    for a, reg, n in chans:
        out += bytes([a, reg, n])
    return out


def dec_stream_read(r):
    """-> (status, dropped, [(ts_us, flags, data)])"""
    assert r[0] == CMD_READ, r
    st, n, size = r[1], r[2], r[3]
    dropped = struct.unpack("<I", bytes(r[4:8]))[0]
    recs = []
    for i in range(n):
        b = bytes(r[8 + i * size:8 + (i + 1) * size])
        recs.append((struct.unpack("<I", b[:4])[0], b[4], b[5:]))
    return st, dropped, recs


def unwrap_ts(rows):
    """Extend the u32 microsecond timestamps of (ts, flags, data) rows to 64 bit."""
    out, off, prev = [], 0, None
    for ts, flags, data in rows:
        if prev is not None and ts < prev and prev - ts > 1 << 31:
            off += 1 << 32
        prev = ts
        out.append((ts + off, flags, data))
    return out


class Probe:
    def __init__(self):
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise ProbeError("pyusb is not installed (pip install pyusb)")
        self._usb = usb
        try:
            dev = usb.core.find(idVendor=VID, idProduct=PID)
        except usb.core.USBError as e:  # e.g. no libusb backend
            raise ProbeError(f"USB access failed: {e}")
        if dev is None:
            raise ProbeError(f"no probe found (USB {VID:04X}:{PID:04X}); is it plugged in, "
                             "and do you have permission to open it?")
        for cfg in dev:
            for intf in cfg:
                try:
                    name = usb.util.get_string(dev, intf.iInterface) or ""
                except (usb.core.USBError, ValueError):
                    name = ""
                if "CMSIS-DAP" not in name:
                    continue

                def ep(direction, intf=intf):
                    return usb.util.find_descriptor(
                        intf, custom_match=lambda e: usb.util.endpoint_direction(
                            e.bEndpointAddress) == direction)
                self.out, self.inp = ep(usb.util.ENDPOINT_OUT), ep(usb.util.ENDPOINT_IN)
                try:
                    usb.util.claim_interface(dev, intf.bInterfaceNumber)
                except usb.core.USBError as e:
                    raise ProbeError(f"cannot claim the probe's CMSIS-DAP interface: {e}")
                return
        raise ProbeError("the probe has no CMSIS-DAP vendor interface")

    def xfer(self, req):
        try:
            self.out.write(req, 1000)
            r = bytes(self.inp.read(64, 1000))
        except self._usb.core.USBError as e:
            raise ProbeError(f"USB transfer failed: {e}")
        if not r or r[0] != req[0]:
            raise ProbeError(f"unexpected reply to command 0x{req[0]:02X}: {r.hex()}")
        return r

    def info(self):
        return dec_info(self.xfer(bytes([CMD_INFO])))

    def pin_names(self):
        names = []
        for i in range(self.info()["npins"]):
            st, name = dec_pin_info(self.xfer(bytes([CMD_PIN_INFO, i])))
            if st == 0:
                names.append(name)
        return names

    def i2c_write(self, addr, data):
        st = self.xfer(enc_i2c(addr, data))[1]
        if st:
            raise ProbeError(f"I2C write to 0x{addr:02X} failed: {status_text(st)}"
                             + (" (is the monitor powered and the address right?)" if st == 1 else ""))

    def i2c_config(self, hz):
        actual = struct.unpack("<I", self.xfer(enc_i2c_config(hz))[1:5])[0]
        if actual == 0:
            raise ProbeError("I2C speed change refused (unavailable or stream busy)")
        return actual

    def _cmd(self, req, what):
        st = self.xfer(req)[1]
        if st:
            raise ProbeError(f"{what}: {status_text(st)}")

    def stream_config(self, period_us, marker, chans):
        self._cmd(enc_stream_config(period_us, marker, chans), "stream config")

    def stream_start(self):
        self._cmd(bytes([CMD_START]), "stream start")

    def stream_stop(self):
        self.xfer(bytes([CMD_STOP]))

    def stream_read(self):
        return dec_stream_read(self.xfer(bytes([CMD_READ])))

    def require_stream(self):
        info = self.info()
        if not info["stream"]:
            raise ProbeError(f"probe firmware (protocol v{info['version']}) has no stream "
                             "support; update the probe firmware")
        return info

