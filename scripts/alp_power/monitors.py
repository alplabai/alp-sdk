# SPDX-License-Identifier: Apache-2.0
"""Power-monitor decoders, keyed by part name.  Only the INA236 is implemented.

INA236 maths (datasheet SBOSA81D; same rules as chips/ina236/ina236.c):
  SHUNT_CAL = 0.00512 / (CURRENT_LSB * R_SHUNT), divided by 4 when ADCRANGE=1,
              15-bit (max 0x7FFF)
  Power[W]  = 32 * CURRENT_LSB * POWER       (no extra 1.6 mV bus factor)
  Current   = signed CURRENT * CURRENT_LSB;  Bus = BUS * 1.6 mV
CURRENT_LSB is matched to the shunt ADC step of the chosen range (full scale
current / 2^15), which gives SHUNT_CAL = 2048 for both ranges.
"""

REG_CONFIG, REG_BUS, REG_POWER, REG_CURRENT, REG_CAL = 0x00, 0x02, 0x03, 0x04, 0x05
FS_V = {"wide": 0.08192, "fine": 0.02048}  # ADCRANGE=0 / ADCRANGE=1
BUS_LSB_V = 0.0016
# CONFIG: bit14 as reset default, ADCRANGE bit12, AVG=1, VBUSCT=VSHCT=140 us,
# MODE=7 (continuous shunt+bus) -> 280 us/conversion, fits a 500 us stream period.
CONFIG_BASE = 0x4007


class Ina236:
    part = "ina236"
    # Only Power is streamed: the chip computes P = V x I itself, energy is the
    # integral of P, and every extra register costs ~50 us of I2C time per
    # sample per rail (the stream period must cover all channels of all rails).
    STREAM_REGS = (REG_POWER,)

    def __init__(self, addr, shunt_ohms, range_="wide"):
        if range_ not in FS_V:
            raise ValueError(f"range must be fine or wide, not {range_!r}")
        if not shunt_ohms > 0:
            raise ValueError("shunt must be > 0 ohm")
        self.addr, self.shunt, self.range = addr, shunt_ohms, range_
        div = 4 if range_ == "fine" else 1
        fs_current = FS_V[range_] / shunt_ohms
        self.cal = round(0.00512 / ((fs_current / 32768) * shunt_ohms) / div)
        if not 0 < self.cal <= 0x7FFF:
            raise ValueError("SHUNT_CAL out of range")
        # back-computed from the register actually written (as chips/ina236 does)
        self.current_lsb = 0.00512 / (self.cal * div * shunt_ohms)

    def configure(self):
        """I2C write payloads [reg, hi, lo] to program CONFIG then CALIBRATION."""
        cfg = CONFIG_BASE | (0x1000 if self.range == "fine" else 0)
        return [bytes([REG_CONFIG, cfg >> 8, cfg & 0xFF]),
                bytes([REG_CAL, self.cal >> 8, self.cal & 0xFF])]

    def stream_channels(self):
        return [(self.addr, reg, 2) for reg in self.STREAM_REGS]

    def decode_reg(self, reg, raw):
        """raw: 2 big-endian bytes -> (kind, SI value)."""
        u = int.from_bytes(raw, "big")
        if reg == REG_POWER:
            return "watts", 32 * self.current_lsb * u
        if reg == REG_CURRENT:
            return "amps", (u - 0x10000 if u & 0x8000 else u) * self.current_lsb
        if reg == REG_BUS:
            return "volts", u * BUS_LSB_V
        raise ValueError(f"register 0x{reg:02X} is not decodable")

    def decode(self, raw):
        """Concatenated bytes of this monitor's streamed channels -> {kind: value}."""
        return dict(self.decode_reg(reg, raw[2 * i:2 * i + 2])
                    for i, reg in enumerate(self.STREAM_REGS))


MONITORS = {"ina236": Ina236}


def parse_monitor(spec):
    """'NAME=ina236@0x4A,shunt=0.02[,range=fine|wide]' -> (name, monitor)."""
    try:
        name, rest = spec.split("=", 1)
        head, *opts = rest.split(",")
        part, addr = head.split("@", 1)
        kv = dict(o.split("=", 1) for o in opts)
        if part not in MONITORS:
            raise ValueError(f"unknown part {part!r} (known: {', '.join(MONITORS)})")
        if set(kv) - {"shunt", "range"} or "shunt" not in kv:
            raise ValueError("needs shunt=OHMS, optional range=fine|wide")
        a = int(addr, 0)
        if not 0 < a < 0x80:
            raise ValueError("address must be a 7-bit I2C address")
        return name, MONITORS[part](a, float(kv["shunt"]), kv.get("range", "wide"))
    except ValueError as e:
        raise ValueError(f"bad --monitor {spec!r}: {e}")


def monitor_header(name, m):
    return {"name": name, "part": m.part, "addr": f"0x{m.addr:02X}",
            "shunt": m.shunt, "range": m.range}


def monitor_from_header(h):
    return h["name"], MONITORS[h["part"]](int(h["addr"], 0), h["shunt"], h["range"])
