# SPDX-License-Identifier: Apache-2.0
"""The provisioning functional test: named checks run on the unit as shipped.

``build(ctx)`` returns the catalogue for this unit (SoM preset, bundle family, carrier and
fixtures of the bench). ``run()`` pushes ONE generated shell script, runs it in ONE remote
invocation and judges every check on the host:

- each check is a shell fragment in a file of its own, run in its own session with a
  timeout; the ``main`` lane runs in the foreground and every other lane in the background;
  each check's output goes to its own file, so nothing interleaves; a check that outlives
  its timeout has its whole process group killed, after its ``restore`` lines ran;
- pass criteria come from ``functest-expect-v2n.yaml`` (plus ``--functest-expect``), the SoM
  preset, the carrier description, the release bundle and ``--pmic-expect``; this module
  holds the commands only;
- a result is ``pass`` / ``pass (<measured>)``, ``fail (<reason>)``, ``skipped (<reason>)``
  or ``unread (<reason>)`` (``lt.unread``, whitespace collapsed). ``fail`` and ``unread``
  of a check that is not informational fail the step. A tool that is missing on the image
  and an expected value nobody defined are ``unread``, never ``pass`` and never ``fail``.

Nothing here writes a device register, EEPROM, OTP, flash or the eMMC, binds or unbinds a
driver, or locks anything. The transient state it changes is restored, also when the check is
killed: ``wlan0`` / ``hci0`` brought up for a scan are put back down, the optional CAN links
get their previous state back. It drops the page cache before the eMMC read (as the write
steps' readbacks do).

NOT YET RUN ON A BENCH. See docs/provisioning-v2n.md, "Functional test coverage".
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import shlex
import time
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

import yaml

from provision import gates
from provision import linux_target as lt
from provision.bench import BenchError

REPO = Path(__file__).resolve().parents[2]
EXPECT_FILE = Path(__file__).with_name("functest-expect-v2n.yaml")
REMOTE_SCRIPT = "/tmp/alp-functest.sh"
SUMMARY_KEY = "test_functional"
# Per-check ledger keys. A prefix of their own: the ledger's test_<name> keys are also entered
# by operators, and a default run must never overwrite one of those.
KEY_PREFIX = "test_ft_"
REASON_MAX = 200
SCRIPT_MARGIN_S = 30          # on top of the slowest lane's summed timeouts
# A codec's broadcast address when its chip manifest carries no such row: TAS2563 page-select
# writes are answered there by every amplifier on the bus, so nothing else may be addressed at it.
SHARED_I2C_FALLBACK = {"tas2563": {0x48}}
_UNIT_RE = r"[\w@.:\\-]+\.(service|socket|mount|target|timer|path|device|scope|slice|swap|automount)"


class Fail(Exception):
    """The check ran and the unit does not meet the criterion."""


class Skip(Exception):
    """The check does not apply here (no fixture, nothing fitted)."""


class Unread(Exception):
    """The check could not be evaluated (tool missing, read error, unparsable output)."""


class NoExpect(Exception):
    """Nobody defined the expected value: ``unread`` for a blocking check, ``skipped`` for an
    informational one. Never a pass."""


@dataclass(frozen=True)
class Check:
    name: str                                   # ledger key: test_ft_<name>
    what: str                                   # one line: what proves the function works
    cmd: str = ""                               # POSIX sh fragment, run on the unit
    judge: Callable[[str], str | None] | None = None   # output -> measured value; raises Fail/Skip/Unread
    timeout_s: int = 5
    est_s: float = 0.1                          # ESTIMATE of the typical run time, not measured
    lane: str = "main"                          # checks of a lane run in order; lanes run concurrently
    fixture: str | None = None                  # bench.yaml functional_test.fixtures.<name>
    host: Callable[[], str | None] | None = None        # host-side check (no cmd), e.g. the PSU reading
    same_as: str | None = None                  # judge the output of that other check (no cmd of its own)
    files: dict[str, str] = field(default_factory=dict)   # helper files written next to the script
    tools: tuple[str, ...] = ()                 # commands the fragment needs; one missing = unread
    setup: str = ""                             # sh run first, in the fragment's own shell: records the state
    restore: str = ""                           # sh run when the fragment exits OR is killed; sees setup's variables
    blocking: bool = False                      # True: blocking even if listed informational


# --------------------------------------------------------------------------
# expected values
# --------------------------------------------------------------------------

def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        out[k] = _merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load_expect(private: Path | None = None) -> dict:
    """The public expected values, with the private file merged over them. A carrier's
    ``not_fitted`` list is the UNION of both files: an override can add to it, never shrink it."""
    public = yaml.safe_load(EXPECT_FILE.read_text(encoding="utf-8")) or {}
    doc = public
    if private is not None:
        over = yaml.safe_load(Path(private).read_text(encoding="utf-8")) or {}
        if not isinstance(over, dict):
            raise ValueError(f"{private}: want a mapping")
        doc = _merge(public, over)
        for name, pub in (public.get("carriers") or {}).items():
            mine = (doc.get("carriers") or {}).get(name)
            if isinstance(mine, dict):
                mine["not_fitted"] = sorted(set(pub.get("not_fitted") or []) | set(mine.get("not_fitted") or []))
    if doc.get("schema") != 1:
        raise ValueError("functional-test expected values: want schema 1")
    return doc


def _want(x: dict, key: str):
    """An expected value, or NoExpect: a criterion nobody has defined is not a pass."""
    cur = x
    for part in key.split("."):
        if not isinstance(cur, dict) or part not in cur or cur[part] is None:
            raise NoExpect(key)
        cur = cur[part]
    return cur


def describe_boot_mode(got: dict) -> str:
    return f"debug_en={got['soc_boot_debug_en']} boot_device={got['soc_boot_device']} md_boot={got['soc_md_boot']} boot_cpu={got['soc_boot_cpu']}"


def judge_boot_mode(word: int, silicon: str, x: dict) -> tuple[dict, bool]:
    """(decoded ledger keys, matches the expectation) for a latched boot-strap word, judged against
    ``boot_mode`` of the expectations file. The single judge of functional_test's boot_mode and
    cold_boot_test. Raises on a missing description/expectation (NoExpect, KeyError, ValueError)."""
    got = lt.decode_lsi_mode(word, lt.sys_lsi_spec(silicon))
    # int(): an overlay may write the flag as bool or 0/1
    ok = (int(got["soc_boot_debug_en"]) == int(_want(x, "boot_mode.debug_enable"))
          and got["soc_boot_device"] == _want(x, "boot_mode.boot_device")
          and got["soc_boot_cpu"] == _want(x, "boot_mode.boot_cpu"))
    return got, ok


# --------------------------------------------------------------------------
# judge helpers
# --------------------------------------------------------------------------

def _kv(out: str) -> dict[str, str]:
    return dict(re.findall(r"(?:^|\s)(\w+)=(\S*)", out))


def _num(out: str, rx: str, what: str) -> float:
    m = re.search(rx, out, re.M)
    if not m:
        raise Unread(f"no {what} in the output: {out.strip()[-80:]!r}")
    return float(int(m[1], 16)) if m[1].lower().startswith("0x") else float(m[1])


def _band(v: float, band, unit: str) -> str:
    lo, hi = band
    if not (math.isfinite(v) and lo <= v <= hi):
        raise Fail(f"{v:g} {unit} outside {lo:g}..{hi:g} {unit}")
    return f"{v:g} {unit}"


def _bytes(out: str, n: int) -> bytes:
    """n bytes of ONE i2ctransfer reply (the first line that carries any)."""
    line = next((ln for ln in out.splitlines() if re.search(r"0x[0-9a-fA-F]{2}\b", ln)), "")
    try:
        return lt._parse_bytes(line, n)
    except BenchError as e:
        raise Unread(str(e) if line else f"no reply: {out.strip()[-80:]!r}") from e


def _rx(pattern: str, text: str, what: str) -> str:
    text = text.strip()
    if not re.search(pattern, text, re.M):
        raise Fail(f"{what} {text[-80:]!r} does not match /{pattern}/")
    return text.splitlines()[-1][:60] if text else ""


# --------------------------------------------------------------------------
# the catalogue
# --------------------------------------------------------------------------

_NETS = 'NETS=$(for n in /sys/class/net/end[0-9]* /sys/class/net/eth[0-9]*; do [ -e "$n" ] && echo "${n##*/}"; done)'
_EMMC = ('E=; for d in /sys/block/mmcblk*; do n=${d##*/}; case $n in *boot*|*rpmb*) continue;; esac; '
         '[ "$(cat $d/device/type 2>/dev/null)" = MMC ] && E=$n; done')
_CAN_PY = """
import socket, struct, sys
tx, rx = (socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) for _ in range(2))
tx.bind((sys.argv[1],)); rx.bind((sys.argv[2],)); rx.settimeout(2.0)
want = bytes(range(0xA0, 0xA8))
tx.send(struct.pack('=IB3x8s', 0x123, 8, want))
cid, n, data = struct.unpack('=IB3x8s', rx.recv(16))
print('id=0x%x data=%s' % (cid & 0x1FFFFFFF, data[:n].hex()))
sys.exit(0 if (cid & 0x1FFFFFFF) == 0x123 and data[:n] == want else 1)
"""
_UART_PY = """
import os, sys, termios, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a); termios.tcflush(fd, termios.TCIOFLUSH)
want, got, end = b'ALP-UART-LOOP-0123456789', b'', time.time() + 2.0
os.write(fd, want)
while len(got) < len(want) and time.time() < end:
    try:
        got += os.read(fd, 64)
    except BlockingIOError:
        time.sleep(0.02)
print('got=%s' % got.hex())
sys.exit(0 if got == want else 1)
"""
_RTC_SET_PY = ("import fcntl,os,struct,sys,time;t=time.gmtime(int(sys.argv[1]));"
               "fd=os.open('/dev/rtc0',os.O_RDONLY);"
               "fcntl.ioctl(fd,0x4024700a,struct.pack('9i',t.tm_sec,t.tm_min,t.tm_hour,t.tm_mday,"
               "t.tm_mon-1,t.tm_year-1900,0,0,0))")
# tests/hil/v2m103-x-evk/v2m103-bt-hci-up.yaml, with the restore moved to the exit trap
_BT_SETUP = "was_up=0; hciconfig hci0 2>/dev/null | grep -q 'UP RUNNING' && was_up=1"
_HIL_BT = """[ -e /sys/class/bluetooth/hci0 ] && echo HIL_BT_HCI_OK
hciconfig hci0 up
for t in 1 2 3 4 5; do
  hciconfig hci0 | grep -q 'UP RUNNING' && { echo HIL_BT_UP_OK; break; }
  [ $t = 5 ] && echo HIL_BT_DOWN || sleep 1
done
hciconfig hci0 | grep 'BD Address'"""
_BT_RESTORE = '[ "${was_up:-1}" = 1 ] || hciconfig hci0 down'
# hcitool, not bluetoothctl: a raw LE scan needs no bluetoothd, so nothing is cached under
# /var/lib/bluetooth (factory neighbours must not ship on the unit) and no earlier scan's
# devices are listed. Its output goes to the script's own temp dir.
_BT_SCAN = """hciconfig hci0 up
hcitool -i hci0 lescan --duplicates >"$D/lescan.txt" 2>&1 &
scan=$!
sleep 5; kill -INT $scan 2>/dev/null; wait $scan 2>/dev/null
sort -u "$D/lescan.txt\""""
# the scan is a child of the killed body: the process-group kill ends it; pkill is the fallback
_BT_SCAN_RESTORE = "command -v pkill >/dev/null 2>&1 && pkill -INT -x hcitool 2>/dev/null; " + _BT_RESTORE
_HIL_OPTIGA = """for i in $(seq 1 100); do
  if i2ctransfer -f -y @BRD@ w1@@ADDR@ 0x82 >/dev/null 2>&1; then
    sleep 0.01
    s=$(i2ctransfer -f -y @BRD@ r4@@ADDR@ 2>/dev/null) && { echo "HIL_OPTIGA_I2C_STATE $s"; echo HIL_OPTIGA_ACK; break; }
  fi
  sleep 0.05
done"""
_HIL_AUDIO = """grep -qF "$C" /proc/asound/cards 2>/dev/null || { echo card=absent; exit 0; }
c=$(awk -v c="$C" 'index($0, c) && $1 ~ /^[0-9]+$/ {print $1; exit}' /proc/asound/cards)
a=$(cut -d' ' -f1 /proc/uptime)
aplay -D hw:$c,0 -f S16_LE -r 48000 -c 2 -d 2 /dev/zero >/dev/null 2>&1; echo "aplay_rc=$?"
b=$(cut -d' ' -f1 /proc/uptime)
awk -v a="$a" -v b="$b" 'BEGIN { print "elapsed=" b - a }'"""
# what the links were: flags (bit 0 = up) and the bitrate, when one was configured
_CAN_SETUP = """for c in can0 can1; do
  [ -e /sys/class/net/$c ] || continue
  eval "f_$c=\\$(cat /sys/class/net/$c/flags)"
  eval "b_$c=\\$(ip -details link show $c | sed -n 's/.*bitrate \\([0-9][0-9]*\\).*/\\1/p' | head -n1)"
done"""
_CAN = """[ -e /sys/class/net/can0 ] && [ -e /sys/class/net/can1 ] || { echo "ALPUNREAD the image has no can0/can1"; exit 0; }
for c in can0 can1; do ip link set $c down 2>/dev/null; ip link set $c type can bitrate 500000 && ip link set $c up; done 2>&1
python3 "$D/can.py" can0 can1 2>&1; echo "rc=$?\""""
# back to what it was: down, the previous bitrate (when there was one), up again only if it was up
_CAN_RESTORE = """for c in can0 can1; do
  eval "f=\\${f_$c:-}; b=\\${b_$c:-}"
  [ -n "$f" ] || continue
  ip link set $c down 2>/dev/null
  [ -n "$b" ] && ip link set $c type can bitrate "$b" 2>/dev/null
  [ $((f & 1)) = 1 ] && ip link set $c up 2>/dev/null
done; true"""


def _preset_devices(preset: dict, bus_name: str) -> list[tuple[str, int, bool]]:
    """(chip, address, optional) of one on-module bus of the SoM preset."""
    spec = ((preset.get("on_module") or {}).get("i2c_devices") or {}).get(bus_name) or {}
    return [(d["chip"], int(str(d["address_7bit"]), 16), d.get("assembled") == "optional")
            for d in spec.get("devices", [])]


def _cpu_count(preset: dict) -> int | None:
    """Application-core count from the SoC description the preset names."""
    try:
        vendor, family, part = str(preset["silicon"]).split(":")
        doc = json.loads((REPO / "metadata" / "socs" / vendor / family / f"{part}.json").read_text(encoding="utf-8"))
        return sum(c["count"] for c in doc["cores"] if str(c.get("type", "")).startswith("cortex-a"))
    except (KeyError, ValueError, OSError):
        return None


def _carrier(name: str) -> dict:
    path = REPO / "metadata" / "boards" / f"{name}.yaml"
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", str(name)) or not path.is_file():
        raise ValueError(f"bench.yaml functional_test.carrier: no metadata/boards/{name}.yaml")
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def _linux_bus(ctx, e1m_bus: str) -> int:
    """The Linux I2C bus number of an E1M bus the carrier description names (``E1M_X_I2C0``),
    through the same preset-bus -> bench.yaml key table the rest of the tool uses."""
    key = lt.PRESET_BUS_TO_BENCH.get(str(e1m_bus).lower().replace("_x_", "_"))
    bus = ctx.bench.i2c_bus.get(key) if key and ctx.bench is not None else None
    if bus is None:
        raise ValueError(f"carrier I2C bus {e1m_bus}: no bench.yaml i2c_bus number for it")
    return bus


def _shared_addresses(board: dict, e1m_bus: str) -> dict[int, str]:
    """{address: chip} of every address on ``e1m_bus`` that is another device's shared /
    broadcast address (from the codec's chip manifest, else SHARED_I2C_FALLBACK). No I2C
    transfer is ever generated to one of these, whatever the expected-values files say."""
    out: dict[int, str] = {}
    for c in (board.get("audio") or {}).get("codecs") or []:
        if c.get("i2c_bus") != e1m_bus:
            continue
        chip, addrs = str(c.get("chip")), set()
        try:
            doc = yaml.safe_load((REPO / "metadata" / "chips" / f"{chip}.yaml").read_text(encoding="utf-8")) or {}
            addrs = {int(r["addr_7bit"]) for r in (doc.get("i2c") or {}).get("addresses") or []
                     if "broadcast" in str(r.get("scope", "")).lower()}
        except (OSError, KeyError, TypeError, ValueError, yaml.YAMLError):
            pass
        for a in addrs or SHARED_I2C_FALLBACK.get(chip, set()):
            out[a] = chip
    return out


def config(ctx) -> dict:
    """bench.yaml ``functional_test:`` -- ``carrier`` and ``fixtures`` (all off by default)."""
    raw = (ctx.bench.raw.get("functional_test") if ctx.bench is not None else None) or {}
    if not isinstance(raw, dict) or not isinstance(raw.get("fixtures") or {}, dict):
        raise ValueError("bench.yaml functional_test: want a mapping with an optional `fixtures` mapping")
    return raw


def side_facts(ctx) -> dict[str, str]:
    """Ledger facts a host-side check measured on the way (the supply voltage)."""
    return dict(ctx._cache.get("functest_facts") or {})


def build(ctx, x: dict | None = None) -> list[Check]:
    """The checks that apply to this unit on this bench. Never touches the unit."""
    x = x if x is not None else (ctx.functest_expect or load_expect())
    cfg = config(ctx)
    fx = cfg.get("fixtures") or {}
    brd = ctx.bench.i2c_bus.get("brd") if ctx.bench is not None else None
    i0 = ctx.bench.i2c_bus.get("eeprom") if ctx.bench is not None else 0     # offline plan: listing only
    if i0 is None:
        raise ValueError("bench.yaml i2c_bus.eeprom is null: the functional test reads the identity EEPROM "
                         "and the carrier on that bus")
    m1 = ctx.family == "v2n-m1"
    q = shlex.quote
    out: list[Check] = []

    def add(name, what, cmd="", judge=None, **kw):
        out.append(Check(name, what, cmd, judge, **kw))

    # ---- boot source, CPU, memory ---------------------------------------------------------
    def j_boot(o):
        kv = _kv(o)
        if not kv.get("emmc"):
            raise Unread("no eMMC found by sysfs device/type")
        if not kv.get("root", "").startswith(kv["emmc"]):
            raise Fail(f"root {kv.get('root') or '?'} is not on the eMMC {kv['emmc']}: "
                       "the unit is not running its shipping image")
        return kv["root"]
    add("boot_source", "the root filesystem is on the eMMC (the unit runs what it ships with)",
        'echo "emmc=$E"; r=$(readlink -f /sys/dev/block/$(mountpoint -d /)); echo "root=${r##*/}"', j_boot,
        tools=("readlink", "mountpoint"))

    def j_cpu(o):
        want = _cpu_count(ctx.preset)
        if want is None:
            raise NoExpect("the SoC description's core count")
        got = int(_num(o, r"^(\d+)$", "processor count"))
        if got != want:
            raise Fail(f"{got} CPUs online, the SoC has {want}")
        return str(got)
    add("cpu_count", "every application core is online", "grep -c '^processor' /proc/cpuinfo", j_cpu)

    def j_mem(o):
        kb = _num(o, r"^(\d+)$", "MemTotal")
        nominal = int((ctx.preset.get("memory") or {}).get("dram_mbit") or 0) * 1024 / 8
        if not nominal:
            raise NoExpect("preset memory.dram_mbit")
        _band(kb / nominal, _want(x, "mem_total_fraction"), "of the SKU DRAM")
        return f"{kb:.0f} kB"
    add("mem_total", "MemTotal is the SKU's DRAM size less the reserved regions",
        "awk '/^MemTotal:/{print $2}' /proc/meminfo", j_mem, tools=("awk",))

    add("kernel_release", "the kernel is the release's", "uname -r",
        lambda o: _rx(_want(x, "kernel_release_regex"), o, "uname -r"), tools=("uname",))

    def j_sku(o):
        if o.strip() != ctx.sku:
            raise Fail(f"/chosen/alp,sku is {o.strip()!r}, want {ctx.sku}")
    add("sku", "U-Boot detected this SKU from the EEPROM and told the kernel",
        "tr -d '\\000' < /proc/device-tree/chosen/alp,sku", j_sku, tools=("tr",))

    # ---- eMMC, xSPI ---------------------------------------------------------------------------
    def j_emmc_size(o):
        size = _num(o, r"^(\d+)$", "block count") * 512
        nominal = int((ctx.preset.get("memory") or {}).get("flash_mbit") or 0) * (1 << 20) / 8
        if not nominal:
            raise NoExpect("preset memory.flash_mbit")
        _band(size / nominal, _want(x, "emmc_size_fraction"), "of the SKU eMMC size")
        return f"{size:.0f} B"
    add("emmc_size", "the eMMC user area is the SKU's size", "cat /sys/block/$E/size", j_emmc_size)

    def j_emmc_health(o):
        a, b, eol = (int(_num(o, rf"(?i){k}[^\n]*?:\s*(0x[0-9a-f]+)", k)) for k in
                     ("life time estimation a", "life time estimation b", "pre eol"))
        val = f"life time A={a:#04x} B={b:#04x}, pre-EOL={eol:#04x}"
        if max(a, b) > _want(x, "emmc_life_time_max") or eol != _want(x, "emmc_pre_eol"):
            raise Fail(val)
        return val
    add("emmc_health", "eMMC life-time and pre-EOL registers say new",
        'o=$(mmc extcsd read /dev/$E 2>&1) || { echo "ALPUNREAD mmc extcsd read rc=$?: $(echo "$o" | tail -n1)"; '
        "exit 0; }\necho \"$o\" | grep -iE 'life time|pre eol'",
        j_emmc_health, timeout_s=10, est_s=0.3, tools=("mmc", "tail"))

    def j_emmc_mode(o):
        if "timing spec" not in o:
            raise Unread("mmc ios not readable (debugfs not mounted?)")
        if _kv(o).get("hs200_failed", "0") != "0":
            raise Fail("mmc_select_hs200 failed in the kernel log")
        return _rx(_want(x, "emmc_timing_regex"), next(ln for ln in o.splitlines() if "timing spec" in ln),
                   "timing").split(":", 1)[-1].strip()
    add("emmc_mode", "the eMMC runs its fast bus mode",
        'h=$(basename "$(dirname "$(readlink -f /sys/block/$E/device)")"); grep "timing spec" '
        "/sys/kernel/debug/$h/ios 2>&1; echo \"hs200_failed=$(dmesg | grep -c 'mmc_select_hs200 failed')\"",
        j_emmc_mode, tools=("basename", "dirname", "readlink", "dmesg"))

    mib = int(x.get("emmc_read_mib") or 64)

    def j_emmc_read(o):
        if f"{mib}+0 records out" not in o:
            raise Fail(f"short or failed read: {o.strip()[-80:]!r}")
        kv = _kv(o)
        try:
            rate = mib / max(float(kv["b"]) - float(kv["a"]), 0.01)
        except (KeyError, ValueError) as e:
            raise Unread("no /proc/uptime stamps") from e
        if rate < _want(x, "emmc_read_min_mib_s"):
            raise Fail(f"{rate:.0f} MiB/s < {x['emmc_read_min_mib_s']} MiB/s")
        return f"{mib} MiB at {rate:.0f} MiB/s"
    # the page cache is dropped first (as the write steps' readbacks do), so the read comes
    # from the media on every run of the check, not only on the first one after a boot
    add("emmc_read", f"a {mib} MiB read of the eMMC user area completes at speed",
        f"{lt.DROP_CACHES}\n"
        f"a=$(cut -d' ' -f1 /proc/uptime); dd if=/dev/$E of=/dev/null bs=1M skip=1024 count={mib} 2>&1 | "
        "grep records; echo \"a=$a b=$(cut -d' ' -f1 /proc/uptime)\"",
        j_emmc_read, timeout_s=20, est_s=2.0, lane="emmc", tools=("dd", "cut", "sync"))

    def j_xspi(o):
        sizes = dict(re.findall(r"^(mtd[01]): ([0-9a-f]{8}) ", o, re.M))
        if set(sizes) != {"mtd0", "mtd1"} or any(int(v, 16) == 0 for v in sizes.values()):
            raise Fail(f"mtd0/mtd1 not both present with a size: {sizes or 'none'}")
        return _rx(_want(x, "xspi_jedec_regex"), _kv(o).get("jedec", ""), "JEDEC ID")
    add("xspi", "the xSPI flash is identified and partitioned (its content is census_final's md5)",
        'cat /proc/mtd; echo "jedec=$(cat /sys/bus/spi/devices/*/spi-nor/jedec_id 2>/dev/null | head -n1)"', j_xspi,
        tools=("head",))

    def j_boot_mode(o):
        try:
            got, ok = judge_boot_mode(int(o.strip().splitlines()[-1], 16), str(ctx.preset["silicon"]), x)
        except (KeyError, ValueError, OSError, IndexError) as e:
            raise Unread(f"SYS_LSI_MODE not readable or not described: {e}") from e
        val = describe_boot_mode(got)
        if not ok:
            raise Fail(val)
        return val
    try:
        _lsi_mode_cmd = lt.devmem_cmd(int(lt.sys_lsi_spec(str(ctx.preset["silicon"]))["registers"]["soc_sys_lsi_mode"], 16))
    except (KeyError, ValueError, OSError):
        _lsi_mode_cmd = "echo 'ALPUNREAD no sys_lsi description for this SoC'"
    add("boot_mode", "the SoC latched normal boot mode, not debug mode (MD_BOOT3 low), from the boot device it ships with",
        _lsi_mode_cmd, j_boot_mode, blocking=True)       # blocking: an overlay cannot demote the debug-mode check

    # ---- Ethernet -----------------------------------------------------------------------------
    nports = int((ctx.preset.get("on_module") or {}).get("ethernet_phy_count") or 2)

    def j_phy(o):
        ids = dict(re.findall(r"^(\w+) (0x[0-9a-f]{8})$", o, re.M))
        if len(ids) < nports:
            raise Unread(f"{len(ids)} of {nports} ports readable: {' '.join(o.split())[-80:]!r}")
        bad = {n: v for n, v in ids.items() if int(v, 16) != _want(x, "eth_phy_id")}
        if bad:
            raise Fail(f"PHY ID {bad}, want {x['eth_phy_id']:#010x}")
        return " ".join(f"{n}={v}" for n, v in sorted(ids.items()))
    add("eth_phy_id", "both PHYs answer on MDIO with the right ID (MII registers 2/3)",
        'python3 "$D/mii.py" $NETS 2>&1', j_phy, est_s=0.3, files={"mii.py": lt.MII_ID_PY}, tools=("python3",))

    def j_mac(o):
        from alp_eth_mac import AlpEthMacError, derive_both_macs   # scripts/ is on sys.path
        try:
            want = [m.lower() for m in derive_both_macs(ctx.serial)]
        except AlpEthMacError as e:
            raise Unread(f"serial {ctx.serial}: {e}") from e
        got = [ln.split()[1].lower() for ln in sorted(o.splitlines()) if len(ln.split()) == 2]
        if got[:nports] != want[:nports]:
            raise Fail(f"MACs {got}, the serial derives {want[:nports]}")
        return " ".join(got[:nports])
    add("eth_mac", "both MACs are the ones derived from the EEPROM serial",
        'for n in $NETS; do echo "$n $(cat /sys/class/net/$n/address)"; done', j_mac)

    link = ('n=$(echo "$NETS" | sed -n @N@p); echo "if=$n carrier=$(cat /sys/class/net/$n/carrier 2>/dev/null) '
            'speed=$(cat /sys/class/net/$n/speed 2>/dev/null) duplex=$(cat /sys/class/net/$n/duplex 2>/dev/null)"; '
            "gw=$(ip route | awk '/^default/{print $3; exit}'); [ -n \"$gw\" ] || gw=${SSH_CLIENT%% *}; "
            'r0=$(cat /sys/class/net/$n/statistics/rx_packets 2>/dev/null); '
            'ping -c 2 -W 2 -I $n "$gw" >/dev/null 2>&1 && echo ping=ok || echo ping=fail; '
            'echo "rx_delta=$(( $(cat /sys/class/net/$n/statistics/rx_packets 2>/dev/null) - r0 ))"')

    def j_link(o):
        kv = _kv(o)
        if kv.get("carrier") != "1":
            raise Fail(f"{kv.get('if')}: no carrier")
        if not kv.get("speed", "").isdigit() or int(kv["speed"]) not in _want(x, "eth_link_speeds"):
            raise Fail(f"{kv.get('if')}: link speed {kv.get('speed') or '?'}")
        if kv.get("duplex") != "full":
            raise Fail(f"{kv.get('if')}: duplex {kv.get('duplex') or '?'}")
        if kv.get("ping") != "ok" or int(kv.get("rx_delta") or 0) < 2:
            raise Fail(f"{kv.get('if')}: no ping reply through this port (rx_delta={kv.get('rx_delta')})")
        return f"{kv['if']} {kv['speed']} Mbit/s full, ping ok"
    net_tools = ("ip", "ping", "awk", "sed")
    add("eth0_link", "port 0: link, speed, duplex and a ping through it (the provisioning network)",
        link.replace("@N@", "1"), j_link, timeout_s=10, est_s=1.5, lane="net", tools=net_tools)
    add("eth1_link", "port 1: link, speed, duplex and a ping through it",
        link.replace("@N@", "2"), j_link, timeout_s=10, est_s=1.5, lane="net", fixture="eth1_cable", tools=net_tools)

    # ---- Wi-Fi, Bluetooth -----------------------------------------------------------------------
    def j_wifi(o):
        if "wlan0=present" not in o:
            raise Fail("no wlan0")
        if "FWBAD" in o:
            raise Fail("firmware load failure: " + next(ln for ln in o.splitlines() if "FWBAD" in ln)[-80:])
        if not _kv(o).get("sdio"):
            raise Fail("no SDIO function enumerated")
        return _rx(_want(x, "wifi_firmware_regex"), o, "firmware banner")[-48:]
    add("wifi_present", "the Wi-Fi module enumerates on SDIO, its firmware loads, wlan0 exists",
        "[ -e /sys/class/net/wlan0 ] && echo wlan0=present; "
        'echo "sdio=$(ls /sys/bus/sdio/devices 2>/dev/null | head -n1)"; '
        "dmesg | grep -iE 'brcmfmac|cyw' | grep -v 'Direct firmware load' | "
        "grep -iE 'firmware.*(fail|error)|fail.*firmware' | sed 's/^/FWBAD /'; "
        "dmesg | grep -iE 'brcmfmac|cyw' | grep 'Firmware: ' | tail -n 1", j_wifi,
        tools=("dmesg", "sed", "head", "tail"))

    def j_reg(o):
        m = re.search(r"^country (\S+?):", o, re.M)
        if not m:
            raise Unread(f"no country line: {o.strip()[-60:]!r}")
        if not re.search(_want(x, "wifi_country_regex"), m[1]):
            raise Fail(f"regulatory domain {m[1]}")
        return f"country {m[1]}"
    add("wifi_regdomain", "the regulatory domain is the expected one", "iw reg get 2>&1", j_reg, tools=("iw",))

    ap = fx.get("wifi_ap")

    def j_scan(o):
        n = len(re.findall(r"^BSS ", o, re.M))
        if n < 1:
            raise Fail(f"the scan saw no network: {o.strip()[-80:]!r}")
        if isinstance(ap, dict) and ap.get("ssid"):
            blocks = re.split(r"(?m)^(?=BSS )", o)
            mine = [b for b in blocks if re.search(rf"SSID: {re.escape(str(ap['ssid']))}\s*$", b, re.M)]
            if not mine:
                raise Fail(f"reference AP {ap['ssid']!r} not among {n} networks")
            sig = max(_num(b, r"signal: (-?[\d.]+)", "signal") for b in mine)
            if sig < float(ap.get("min_signal_dbm", -70)):
                raise Fail(f"reference AP at {sig:g} dBm < {float(ap.get('min_signal_dbm', -70)):g} dBm")
            return f"{n} networks, reference AP {sig:g} dBm"
        return f"{n} networks"
    add("wifi_scan", "a Wi-Fi scan sees a network (the RF receive path works)",
        "ip link set wlan0 up\n"
        "s=$(iw dev wlan0 scan 2>&1) || { sleep 2; s=$(iw dev wlan0 scan 2>&1); }\n"
        "echo \"$s\" | grep -E '^BSS |signal:|SSID:|busy|failed'",
        j_scan, timeout_s=20, est_s=5.0, lane="radio", fixture="wifi_ap", tools=("iw", "ip"),
        setup="f=$(cat /sys/class/net/wlan0/flags 2>/dev/null)", restore="[ $(( ${f:-1} & 1 )) = 1 ] || ip link set wlan0 down")

    def j_bt(o):
        if "HIL_BT_HCI_OK" not in o:
            raise Fail("no hci0")
        if "HIL_BT_DOWN" in o or "HIL_BT_UP_OK" not in o:
            raise Fail("hci0 does not come UP")
        if re.search(r"BD Address: 00:00:00:00:00:00", o):
            raise Fail("hci0 has no BD address")
        return "UP RUNNING"
    add("bt_hci", "the Bluetooth controller enumerates on its UART and powers up",
        _HIL_BT, j_bt, timeout_s=12, est_s=1.0, lane="radio", tools=("hciconfig",), setup=_BT_SETUP, restore=_BT_RESTORE)

    adv = fx.get("ble_advertiser")

    def j_bt_scan(o):
        seen = {a.upper() for a in re.findall(r"^([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b", o, re.M)}
        if not seen and re.search(r"(?i)failed|error|denied|no such device", o):
            raise Unread(f"the scan did not run: {' '.join(o.split())[-80:]}")
        if not seen:
            raise Fail("the scan saw no advertiser")
        if isinstance(adv, dict) and adv.get("address") and str(adv["address"]).upper() not in seen:
            raise Fail(f"reference advertiser {adv['address']} not among {len(seen)} devices")
        return f"{len(seen)} devices"
    add("bt_scan", "a Bluetooth LE scan sees an advertiser (the RF receive path works)",
        _BT_SCAN, j_bt_scan, timeout_s=20, est_s=6.0, lane="radio", fixture="ble_advertiser",
        tools=("hciconfig", "hcitool", "sort"), setup=_BT_SETUP, restore=_BT_SCAN_RESTORE)

    # ---- DX-M1, DRP-AI, GPU ---------------------------------------------------------------------
    if m1:
        def j_pcie(o):
            kv = _kv(o)
            if kv.get("present") != "1":
                raise Fail("no PCIe endpoint at 0000:01:00.0 (link down)")
            if kv.get("device") != lt.DXM1_PCIE_FW_RUNNING:
                raise Fail(f"PCIe device {kv.get('device')}, want {lt.DXM1_PCIE_FW_RUNNING} (firmware running)")
            _rx(_want(x, "dxm1_pcie_driver_regex"), kv.get("driver", ""), "bound driver")
            if not kv.get("width", "").isdigit() or not kv.get("speed"):
                raise Unread(f"current_link_width / current_link_speed not readable: {' '.join(o.split())[-80:]!r}")
            if int(kv["width"]) != _want(x, "dxm1_link_width"):
                raise Fail(f"PCIe link width x{kv['width']}, want x{x['dxm1_link_width']}")
            speed = kv["speed"].replace("_", " ")
            if not re.search(_want(x, "dxm1_link_speed_regex"), speed):
                raise Fail(f"PCIe link speed {speed!r} does not match /{x['dxm1_link_speed_regex']}/")
            return f"x{kv['width']} {speed} {kv['driver']}"
        add("dxm1_pcie", "the DX-M1 enumerates at 0000:01:00.0, firmware running, driver bound, full link width and speed",
            "d=/sys/bus/pci/devices/0000:01:00.0; [ -e $d ] || { echo present=0; exit 0; }\n"
            'echo "present=1 device=$(cat $d/device) width=$(cat $d/current_link_width 2>/dev/null) '
            "speed=$(cat $d/current_link_speed 2>/dev/null | tr ' ' _)\"\n"
            '[ -e $d/driver ] && echo "driver=$(basename $(readlink $d/driver))" || echo driver=none', j_pcie,
            tools=("basename", "readlink", "tr"))

        fw = next((c for c in ctx.bundle.get("components", []) if c.get("role") == "dxm1_fw"), {}).get("version")

        def j_dxrt(o):
            if "dev=ok" not in o:
                raise Fail("no /dev/dxrt0")
            if _kv(o).get("service") != "active":
                raise Fail(f"dxrt.service is {_kv(o).get('service') or '?'}")
            got = lt.parse_dxm1_fw_version(o)
            if got is None:
                raise Unread("dxrt-cli -s printed no firmware version")
            want = str(fw or x.get("dxm1_fw_version") or "").strip().lstrip("vV")
            if want and got != want:
                raise Fail(f"DX-M1 firmware {got}, want {want}")
            return f"firmware {got}"
        add("dxm1_runtime", "the DX-M1 runtime talks to the device and reports the release's firmware version",
            'test -c /dev/dxrt0 && echo dev=ok; echo "service=$(systemctl is-active dxrt.service 2>/dev/null)"; '
            "dxrt-cli -s 2>&1", j_dxrt, timeout_s=20, est_s=2.0, lane="npu", tools=("dxrt-cli", "systemctl"))

        model = fx.get("dxm1_model")
        model = model.get("model") if isinstance(model, dict) else model

        def j_infer(o):
            if _kv(o).get("rc") != "0":
                raise Fail(f"run_model rc={_kv(o).get('rc')}: {' '.join(o.split())[-100:]!r}")
            return _rx(_want(x, "dxm1_inference_pass_regex"), o.rsplit("rc=", 1)[0], "run_model output")
        add("dxm1_inference", "a short inference on the DX-M1 completes",
            f'M={q(model) if isinstance(model, str) else q("")}; [ -r "$M" ] || '
            '{ echo "ALPUNREAD model $M is not on the unit"; exit 0; }\n'
            f'o=$(run_model -m "$M" -l {int(x.get("dxm1_inference_loops") or 30)} 2>&1); rc=$?; '
            'echo "$o" | tail -n 20; echo "rc=$rc"',
            j_infer, timeout_s=40, est_s=8.0, lane="npu", fixture="dxm1_model", tools=("run_model", "tail"))

    def j_bound(node: str = ""):
        def j(o):
            if "driver=bound" not in o:
                raise Fail("no driver bound")
            if node and node not in o:
                raise Fail(f"no {node}* device node")
            return node or "driver bound"
        return j
    pd = x.get("platform_devices") or {}

    def bound(dev: str) -> str:
        return f"[ -e {q('/sys/bus/platform/devices/' + str(dev) + '/driver')} ] && echo driver=bound"
    if pd.get("drpai"):
        add("drpai", "the DRP-AI driver probed and its device node exists",
            f"{bound(pd['drpai'])}; ls /dev/drpai* 2>/dev/null", j_bound("/dev/drpai"))
    if pd.get("gpu"):
        node = str(x.get("gpu_device_node") or "/dev/mali0")
        add("gpu", "the GPU driver probed and its device node exists",
            f"{bound(pd['gpu'])}; ls {q(node)} 2>/dev/null", j_bound(node))

    # ---- board-management I2C: RTC, sensors, secure element, bridge -------------------------------
    if brd is not None:
        devs = _preset_devices(ctx.preset, "brd_i2c")
        addr = {chip: a for chip, a, _opt in devs}
        ids = x.get("i2c_ids") or {}

        if "rv3028c7" in addr:
            rtc = addr["rv3028c7"]
            add("rtc_device", "the RTC is bound as rtc0", "cat /sys/class/rtc/rtc0/name 2>&1",
                lambda o: _rx(_want(x, "rtc_name_regex"), o, "rtc0 name"))

            def j_ticks(o):
                kv = _kv(o)
                try:
                    a, b = (int(kv[k], 16) for k in ("a", "b"))
                except (KeyError, ValueError) as e:
                    raise Unread(f"seconds register: {o.strip()[-60:]!r}") from e
                bcd = lambda v: (v >> 4) * 10 + (v & 0xF)   # noqa: E731
                if (a & 0xF) > 9 or (b & 0xF) > 9 or max(a, b) > 0x59:
                    raise Fail(f"seconds register not BCD: {a:#04x} {b:#04x}")
                d = (bcd(b) - bcd(a)) % 60
                if not 1 <= d <= 4:
                    raise Fail(f"seconds went {a:#04x} -> {b:#04x} across a 2 s sleep")
                return f"+{d} s"
            # the seconds register, not rtc0: it counts whether or not the clock was ever set
            add("rtc_ticks", "the RTC oscillator runs: the seconds register advances",
                f'a=$(i2cget -f -y {brd} {rtc:#04x} 0x00 2>&1) || {{ echo "ALPUNREAD $a"; exit 0; }}; sleep 2; '
                f'echo "a=$a b=$(i2cget -f -y {brd} {rtc:#04x} 0x00 2>&1)"',
                j_ticks, timeout_s=8, est_s=2.2, lane="rtc", tools=("i2cget",))

            def j_set(o):
                if not o.strip().isdigit():
                    raise Fail("clock not set (the RTC's power-on flag is up; nothing set it during provisioning)")
                return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(int(o.strip())))
            add("rtc_time_set", "the RTC holds a valid time", "cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null", j_set)

            def j_bsm(o):
                v = int(_num(o, r"^(0x[0-9a-fA-F]{2})$", "register 0x37"))
                bsm = (v >> 2) & 3
                if bsm not in _want(x, "rtc_bsm_enabled"):
                    raise Fail(f"backup switchover disabled (reg 0x37={v:#04x}, BSM=0b{bsm:02b}): "
                               "the RTC stops when the supply is cut")
                return f"BSM=0b{bsm:02b}"
            add("rtc_backup_mode", "the RTC switches to its backup supply when power is cut",
                f"i2cget -f -y {brd} {rtc:#04x} 0x37 2>&1", j_bsm, tools=("i2cget",))

            def j_keep(o):
                kv = _kv(o)
                was = str(ctx.facts.get("rtc_set_boot_id") or ((ctx.state.get("steps") or {}).get(
                    "cold_boot_test") or {}).get("evidence", {}).get("rtc_set_boot_id") or "")
                if not was:
                    raise Fail("the RTC was not set before the last cold cycle (cold_boot_test sets it after "
                               "its first cycle when this fixture is on and --cold-cycles >= 2)")
                if kv.get("hctosys_failed", "0") != "0":
                    raise Fail("the RTC lost its time across the power cut (this boot's kernel log: "
                               "hctosys: unable to read the hardware clock)")
                if not kv.get("epoch", "").isdigit():
                    raise Fail("the RTC lost its time across the power cut (clock unreadable: power-on flag)")
                if not kv.get("boot"):
                    raise Unread("no boot id")
                if kv["boot"] == was:
                    raise Fail("no reboot since the RTC was set: nothing proven")
                err = abs(int(kv["epoch"]) - time.time())
                if err > _want(x, "rtc_max_error_s"):
                    raise Fail(f"RTC is {err:.0f} s from the host clock after the cold cycles")
                return f"{err:.0f} s from the host clock"
            add("rtc_retention", "the RTC kept time across the cold cycles (backup supply works)",
                'echo "epoch=$(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null)"; '
                'echo "boot=$(cat /proc/sys/kernel/random/boot_id)"; '
                "echo \"hctosys_failed=$(dmesg | grep -c 'hctosys: unable to read the hardware clock')\"",
                j_keep, fixture="rtc_backup", tools=("dmesg",))

        if "tmp112" in addr:
            def j_tmp(o):
                b = _bytes(o, 2)
                raw = int.from_bytes(b, "big", signed=True) >> 4
                return _band(raw * 0.0625, _want(x, "board_temp_c"), "degC")
            add("board_temp", "the on-module temperature sensor reads a plausible temperature",
                lt.xfer_cmd(brd, addr["tmp112"], b"\x00", 2, force=True) + " 2>&1", j_tmp, tools=("i2ctransfer",))

        if "optiga_trust_m" in addr:
            def j_se(o):
                if "HIL_OPTIGA_ACK" not in o:
                    raise Fail("no answer to the I2C_STATE read in 100 tries")
                return "I2C_STATE read"
            add("secure_element", "the secure element answers its protocol-level state read",
                _HIL_OPTIGA.replace("@BRD@", str(brd)).replace("@ADDR@", f"{addr['optiga_trust_m']:#04x}"),
                j_se, timeout_s=12, est_s=0.5, lane="se", tools=("i2ctransfer", "seq"))

        if "gd32g553" in addr:
            def j_gd32(o):
                try:
                    ver = "%d.%d.%d" % lt.gd32_parse_version(o)
                except BenchError as e:
                    raise (Unread if "i2ctransfer returned" in str(e) else Fail)(str(e)) from e
                if ver != str(_want(x, "gd32_protocol")):
                    raise Fail(f"bridge protocol {ver}, want {x['gd32_protocol']}")
                return f"protocol {ver}"
            add("gd32_bridge", "the bridge MCU answers GET_VERSION with a good CRC and the release's protocol",
                lt.gd32_version_cmd(brd, addr["gd32g553"]) + " 2>&1", j_gd32, tools=("i2ctransfer",))

            def j_chip(o):
                n = int(_num(o, r"lines=(\d+)", "gpiochip gd32-bridge-gpio"))
                if n < _want(x, "gd32_gpio_lines_min"):
                    raise Fail(f"{n} lines < {x['gd32_gpio_lines_min']}")
                return f"{n} lines"
            add("gd32_gpiochip", "the bridge's GPIO expander is registered with the kernel",
                'for c in /sys/class/gpio/gpiochip*; do [ "$(cat $c/label 2>/dev/null)" = gd32-bridge-gpio ] '
                '&& echo "lines=$(cat $c/ngpio)"; done', j_chip)

        for chip, a, optional in devs:
            if chip in ids and not optional:
                _add_id(out, x, f"i2c_{chip}_{a:02x}", f"on-module {chip} at {a:#04x} answers"
                        + (" with its ID" if "value" in ids[chip] or a in (ids[chip].get("values") or {}) else ""),
                        brd, a, ids[chip])

        pm = ctx.expected_registers

        def j_pmic(o):
            if not pm:
                raise NoExpect("--pmic-expect")
            got = {(d, int(a, 16), int(r, 16)): v for d, a, r, v in re.findall(r"^R (\w+) (\w+) (\w+) (.*)$", o, re.M)}
            bad, n = [], 0
            for dev, d in (pm.get("devices") or {}).items():
                if d.get("families") and ctx.family not in d["families"]:
                    continue
                for r in d.get("registers", []):
                    n += 1
                    v = got.get((dev, int(d["addr"]), int(r["reg"])), "").strip()
                    mask = int(r.get("mask", 0xFF))
                    if not re.fullmatch(r"0x[0-9a-fA-F]{2}", v):
                        raise Unread(f"{dev} reg {int(r['reg']):#04x}: {v[-60:] or 'no reply'}")
                    if int(v, 16) & mask != int(r["expect"]) & mask:
                        bad.append(f"{dev} reg {int(r['reg']):#04x}={v}, want {int(r['expect']):#04x}")
            if bad:
                raise Fail("; ".join(bad))
            return f"{n} registers"
        lines = []
        for dev, d in ((pm or {}).get("devices") or {}).items():
            if d.get("families") and ctx.family not in d["families"]:
                continue
            bus = ctx.bench.i2c_bus.get(d["bus"])
            for r in d.get("registers", []):
                a, reg = int(d["addr"]), int(r["reg"])
                if not re.fullmatch(r"\w+", str(dev)):
                    raise ValueError(f"--pmic-expect device name {dev!r}: want letters, digits, underscore")
                lines.append(f'echo "R {dev} {a:#04x} {reg:#04x} $(i2cget -f -y {int(bus)} {a:#04x} {reg:#04x} 2>&1)"')
        add("pmic_registers", "the PMIC registers of --pmic-expect hold after a plain cold boot",
            "\n".join(lines) or "true", j_pmic, timeout_s=15, est_s=0.6, tools=("i2cget",) if lines else ())

    # ---- thermal, CM33, USB, SD, system ---------------------------------------------------------
    def j_thermal(o):
        zones = re.findall(r"^Z (\S+) (-?\d+)$", o, re.M)
        if not zones:
            raise Unread("no thermal zone reports")
        lo, hi = _want(x, "soc_temp_c")
        bad = [f"{n}={int(t) / 1000:g}" for n, t in zones if not lo * 1000 <= int(t) <= hi * 1000]
        if bad:
            raise Fail(f"{' '.join(bad)} degC outside {lo:g}..{hi:g} degC")
        return " ".join(f"{int(t) / 1000:g}" for _n, t in zones) + " degC"
    add("thermal", "every SoC thermal zone reads a plausible temperature",
        'for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/temp 2>/dev/null) && echo "Z ${z##*/} $t"; done',
        j_thermal)

    off = gates.CM33_REGION_OFFSET
    cm33 = next((c for c in ctx.bundle.get("components", []) if c.get("role") == "cm33"), None)
    if cm33 is not None:
        # The bundle carries a CM33 image: the unit must hold exactly it. Blocking, whatever
        # the informational list says.
        size = len(ctx.artefact_bytes("cm33"))
        whole, rem = divmod(size, 4096)
        parts = ([f"dd if=/dev/mtd1 bs=4096 skip={off // 4096} count={whole} 2>/dev/null"] if whole else []) + \
                ([f"dd if=/dev/mtd1 bs=1 skip={off + whole * 4096} count={rem} 2>/dev/null"] if rem else [])

        def j_cm33(o):
            got = (o.split() or [""])[0]
            if not re.fullmatch(r"[0-9a-f]{32}", got) or got == hashlib.md5(b"").hexdigest():
                raise Unread(f"mtd1 not readable: {o.strip()[-60:]!r}")
            if got != hashlib.md5(ctx.artefact_bytes("cm33")).hexdigest():
                raise Fail(f"mtd1+{off:#x} md5 {got} is not the bundle's CM33 image")
            return f"md5 {got}"
        add("cm33_firmware", "the CM33 image region of the xSPI flash holds the bundle's CM33 image",
            "{ " + "; ".join(parts or ["true"]) + "; } | md5sum", j_cm33, timeout_s=30, est_s=1.0,
            tools=("dd", "md5sum"), blocking=True)
    else:
        blank = hashlib.md5(b"\xff" * 65536).hexdigest()

        def j_cm33(o):
            got = (o.split() or [""])[0]
            if not re.fullmatch(r"[0-9a-f]{32}", got) or got == hashlib.md5(b"").hexdigest():
                raise Unread(f"mtd1 not readable: {o.strip()[-60:]!r}")
            if got == blank:
                raise Fail(f"mtd1+{off:#x} is blank: no CM33 image on the unit")
            return "image present"
        add("cm33_firmware", "the CM33 image region of the xSPI flash is programmed",
            f"dd if=/dev/mtd1 bs=4096 skip={off // 4096} count=16 2>/dev/null | md5sum", j_cm33,
            tools=("dd", "md5sum"))

    # The CM33 stock image (firmware/alp-stock-shim) writes a beacon into the rsctbl window the A55
    # DT reserves: magic, version, ~1 Hz counter. Read-only /dev/mem reads; a counter that moved
    # proves the core is running, not that it ran once.
    def j_cm33_running(o):
        m = re.search(r"^B (\S+) (\S+) (\S+)$", o, re.M)
        c = re.search(r"^C (\S+)$", o, re.M)
        try:
            magic, ver, c0 = (int(v, 16) for v in m.groups())
            c1 = int(c[1], 16)
        except (AttributeError, ValueError, TypeError):
            raise Unread(f"beacon not readable: {o.strip()[-80:]!r}") from None
        b = _want(x, "cm33_beacon")
        got = f"magic {magic:#010x}, version {ver:#x}, counter {c0} then {c1}"
        if magic != b["magic"] or ver != b["version"]:
            raise Fail(f"{got}: want magic {b['magic']:#010x}, version {b['version']:#x}")
        lo, hi = b["heartbeat_advance"]
        if not lo <= (c1 - c0) & 0xFFFFFFFF <= hi:
            raise Fail(f"{got}: the counter must advance by {lo}..{hi} in 2 s")
        return f"counter {c0} then {c1}"
    addr = (x.get("cm33_beacon") or {}).get("address", 0)      # the judge refuses a missing expectation
    add("cm33_running", "the CM33 liveness beacon is present and its counter advances",
        lt.DEVMEM_READ_FN +
        f'echo "B $(r {addr:#x}) $(r {addr + 4:#x}) $(r {addr + 8:#x})"; sleep 2; echo "C $(r {addr + 8:#x})"',
        j_cm33_running, timeout_s=15, est_s=2.2, blocking=cm33 is not None)

    def j_uio(o):
        missing = [n for n in _want(x, "openamp_uio_names") if n not in o.split()]
        if missing:
            raise Fail(f"missing UIO nodes: {' '.join(missing)}")
        return f"{len(x['openamp_uio_names'])} UIO nodes"
    add("openamp_uio", "the A55 side of the CM33 link (OpenAMP UIO nodes) is present",
        "cat /sys/class/uio/uio*/name 2>/dev/null", j_uio)

    def j_usb(o):
        n = int(_num(o, r"^(\d+)$", "root hub count"))
        if n < _want(x, "usb_root_hubs_min"):
            raise Fail(f"{n} USB root hubs < {x['usb_root_hubs_min']}")
        return f"{n} root hubs"
    add("usb_host", "every USB host controller probed", "ls -d /sys/bus/usb/devices/usb* 2>/dev/null | wc -l", j_usb,
        tools=("wc",))

    def j_read(what: str):
        def j(o):
            kv = _kv(o)
            if not kv.get("dev"):
                raise Fail(f"no {what} found")
            if "4+0 records out" not in o:
                raise Fail(f"{kv['dev']}: read failed: {o.strip()[-60:]!r}")
            return f"{kv['dev']} 4 MiB read"
        return j
    add("usb_device", "a USB device enumerates on the host port and a mass-storage read completes",
        'for b in /sys/block/sd?; do [ -e $b ] || continue; echo "dev=${b##*/}"; '
        "dd if=/dev/${b##*/} of=/dev/null bs=1M count=4 2>&1 | grep records; break; done",
        j_read("USB mass-storage device"), timeout_s=15, est_s=1.0, lane="usb", fixture="usb_stick", tools=("dd",))
    if pd.get("sd"):
        add("sd_host", "the SD slot's host controller probed (the slot itself ran the provisioning image)",
            bound(pd["sd"]), j_bound())
    add("sd_card", "a card in the SD slot is detected and read",
        'for b in /sys/block/mmcblk*; do n=${b##*/}; case $n in *boot*|*rpmb*) continue;; esac; '
        '[ "$(cat $b/device/type 2>/dev/null)" = SD ] || continue; echo "dev=$n"; '
        "dd if=/dev/$n of=/dev/null bs=1M count=4 2>&1 | grep records; break; done",
        j_read("SD card"), timeout_s=15, est_s=1.0, lane="usb", fixture="sd_card", tools=("dd",))

    def j_units(o):
        allow = set(x.get("systemd_failed_allow") or [])
        rows = [ln.split()[0] for ln in o.splitlines() if ln.split()]
        odd = [r for r in rows if not re.fullmatch(_UNIT_RE, r)]
        if odd:                                  # e.g. "Failed to connect to bus": no verdict
            raise Unread(f"unexpected systemctl output: {' '.join(o.split())[:80]}")
        bad = [r for r in rows if r not in allow]
        if bad:
            raise Fail(f"failed units: {' '.join(bad)}")
        return "no failed unit"
    add("systemd_failed", "systemd reports no failed unit (allowlist in the expected values)",
        'o=$(systemctl --failed --no-legend --plain 2>&1) || { echo "ALPUNREAD systemctl rc=$?: $o"; exit 0; }\n'
        'echo "$o"', j_units, est_s=0.3, tools=("systemctl",))

    def j_fatal(o):
        if not o.strip():
            raise Unread("empty kernel log")       # an empty log carries no fault line, and proves nothing
        for pat in x.get("dmesg_deny") or []:
            if m := re.search(rf"^.*(?:{pat}).*$", o, re.M):
                raise Fail(f"kernel log: {m[0].strip()[-120:]}")
        return f"{len(o.splitlines())} lines"

    def j_clean(o):
        lines = o.splitlines()
        if not lines:
            raise Unread("empty kernel log")
        if any(re.match(r"<\d+>", ln) for ln in lines):
            errs = [ln for ln in lines if (m := re.match(r"<(\d+)>", ln)) and int(m[1]) & 7 <= 3]
        else:                                    # no levels (dmesg without -r): fall back to error words
            errs = [ln for ln in lines if re.search(_want(x, "dmesg_error_regex"), ln)]
        bad = [ln for ln in errs if not any(re.search(p, ln) for p in x.get("dmesg_allow") or [])]
        # a stack dump belongs to the line that announced it
        bad = [ln for ln in bad if not re.search(r"\+0x[0-9a-f]+/0x[0-9a-f]+|Call trace:|^\S*\s*\[[\d. ]+\] (CPU:|Hardware "
                                                r"name:|handlers:|\[<)", ln)]
        if bad:
            raise Fail(f"{len(bad)} unexpected error line(s), first: {bad[0].strip()[-120:]}")
        return f"{len(errs)} error-level lines, all known"
    add("dmesg_fatal", "the kernel log carries none of the known fault signatures",
        "dmesg -r 2>/dev/null || dmesg", j_fatal, est_s=0.3, tools=("dmesg",))
    add("dmesg_clean", "the kernel log has no error-level line outside the allowlist", judge=j_clean,
        same_as="dmesg_fatal")

    def j_keys(o):
        if "gpio-keys" not in o:
            raise Fail("no gpio-keys input device")
        return "registered"
    add("gpio_keys", "the carrier push button's input device is registered (a key press needs an operator)",
        "cat /sys/class/input/input*/name 2>/dev/null", j_keys)

    # ---- identity ------------------------------------------------------------------------------
    def j_manifest(o):
        arr = _bytes(o, lt.MANIFEST_LEN)
        ref = ctx.unit_dir / f"{ctx.serial}.manifest.bin"
        if ref.is_file():
            if ref.read_bytes() != arr:
                raise Fail(f"the array differs from the committed {ref.name}")
            return "equals the committed manifest"
        if arr[:4] != b"HPLA" or zlib.crc32(arr[:0x7C]) != int.from_bytes(arr[0x7C:0x80], "little"):
            raise Fail("no valid manifest (magic or CRC)")
        sku = arr[24:48].split(b"\0", 1)[0].decode("ascii", "replace")
        serial = arr[56:68].split(b"\0", 1)[0].decode("ascii", "replace")
        if (sku, serial) != (ctx.sku, ctx.serial):
            raise Fail(f"manifest is for {sku} {serial}")
        return "valid, this unit's SKU and serial"
    add("eeprom_manifest", "the identity EEPROM reads back this unit's manifest",
        lt.xfer_cmd(i0, lt.EEPROM_ADDR, b"\x00\x00", lt.MANIFEST_LEN) + " 2>&1", j_manifest, est_s=0.2,
        tools=("i2ctransfer",))

    frame = gates.identity_frame(gates.IdentityOp.SECURE_PAGE_READ)     # the only 0x58 frame used here

    def j_page(o):
        page = _bytes(o, frame.read_len)
        ref = ctx.unit_dir / f"{ctx.serial}.secure-page.staged.bin"
        if ref.is_file():
            if ref.read_bytes() != page:
                raise Fail(f"the secure page differs from {ref.name}")
            return "equals the staged secure page"
        if page == b"\xff" * len(page):
            raise Fail("the secure page is blank")
        return "written"
    add("secure_page", "the secure data page reads back this unit's mirror",
        lt.xfer_cmd(i0, frame.addr, bytes(frame.write), frame.read_len) + " 2>&1", j_page, est_s=0.2,
        tools=("i2ctransfer",))

    # ---- supply power (host side) ----------------------------------------------------------------
    def h_power():
        power = ctx.bench.power if ctx.bench is not None else None
        amps = power.current() if power is not None else None
        if amps is None:
            raise Skip("no fixture: a supply that measures current (bench.yaml power.kind scpi)")
        if not math.isfinite(amps):
            raise Unread(f"current is not a finite number: {amps}")
        # the band is a power band: the current alone means nothing without the supply voltage
        try:
            volts = power.voltage()
        except BenchError as e:
            raise Unread(f"supply voltage not readable: {e}") from e
        if volts is None or not math.isfinite(volts):
            raise Unread(f"supply voltage not readable: {volts}")
        ctx._cache["functest_facts"] = {"psu_voltage_v": f"{volts:.3f}", "psu_current_a": f"{amps:.3f}",
                                        "psu_current_state": "at-functional-test"}
        val = _band(round(volts * amps, 3), _want(x, f"supply_power_idle_w.{ctx.family}"), "W")
        return f"{val}: {volts:.2f} V x {amps:.3f} A"
    add("supply_power_idle", "the supply power at Linux idle is in the expected band", host=h_power, est_s=0.3)

    out.extend(_carrier_checks(ctx, x, cfg))
    return out


def _add_id(out: list[Check], x: dict, name: str, what: str, bus: int, addr: int, spec: dict, **kw) -> None:
    n = int(spec["read"])
    at, width = int(spec.get("at", 0)), int(spec.get("width", n - int(spec.get("at", 0))))
    want = (spec.get("values") or {}).get(addr, spec.get("value"))

    def judge(o):
        got = int.from_bytes(_bytes(o, n)[at:at + width], "big")
        if want is not None and got != int(want):
            raise Fail(f"ID {got:#0{2 + 2 * width}x}, want {int(want):#0{2 + 2 * width}x}")
        return f"{got:#0{2 + 2 * width}x}"
    out.append(Check(name, what, lt.xfer_cmd(bus, addr, bytes((int(spec["reg"]),)), n, force=True) + " 2>&1",
                     judge, tools=("i2ctransfer",), **kw))


def _carrier_checks(ctx, x: dict, cfg: dict) -> list[Check]:
    """Checks of the carrier the unit sits in: they exercise the SoM pins that reach it."""
    name = cfg.get("carrier")
    if not name:
        def none(_o=None):
            raise Skip("no fixture: bench.yaml functional_test.carrier")
        return [Check("carrier", "the carrier's devices answer through the SoM's pins", host=none)]
    board = _carrier(str(name))
    cx = (x.get("carriers") or {}).get(name) or {}
    fx = cfg.get("fixtures") or {}
    ids = x.get("i2c_ids") or {}
    e1m_bus = str(board.get("i2c_devices_bus") or "")
    bus = _linux_bus(ctx, e1m_bus) if ctx.bench is not None else 0
    shared = _shared_addresses(board, e1m_bus)
    q = shlex.quote
    out: list[Check] = []

    for d in board.get("i2c_devices") or []:
        part, a = d["part"], int(str(d["address"]), 16)
        # never address another device's shared / broadcast address, whatever the expected values say
        if a in shared or a in (cx.get("not_fitted") or []) or part not in ids:
            continue
        gate = (cx.get("fixture") or {}).get(a)
        _add_id(out, x, f"carrier_{part}_{a:02x}", f"carrier {part} at {a:#04x} answers"
                + (" with its ID" if "value" in ids[part] else ""), bus, a, ids[part], fixture=gate)
        rail = (cx.get("rails") or {}).get(a)
        if not rail:
            continue
        ohms = float((d.get("calibration") or {}).get("shunt_ohms") or 0)
        amax = float((d.get("calibration") or {}).get("max_current_a") or 0)
        if part == "ina236":
            def j_rail(o, rail=rail, ohms=ohms, amax=amax):
                rows = [ln for ln in o.splitlines() if "0x" in ln]
                if len(rows) != 3:
                    raise Unread(f"want 3 register reads, got {len(rows)}: {o.strip()[-60:]!r}")
                cfg_reg, bus_raw, shunt = (int.from_bytes(_bytes(r, 2), "big") for r in rows)
                volts = bus_raw * 1.6e-3                                   # bus LSB 1.6 mV
                lsb = 0.625e-6 if cfg_reg & 0x1000 else 2.5e-6             # ADCRANGE
                val = _band(round(volts, 3), rail["volts"], "V")
                if ohms:
                    # magnitude only: which way the shunt is wired is not pinned here
                    amps = abs((shunt - 0x10000 if shunt & 0x8000 else shunt) * lsb / ohms)
                    val += ", " + _band(round(amps, 3), rail.get("amps") or [0.0, amax or 10.0], "A")
                return val
            cmd = "; ".join(lt.xfer_cmd(bus, a, bytes((r,)), 2, force=True) + " 2>&1" for r in (0x00, 0x02, 0x01))
        else:                                                              # ina228: 24-bit VBUS, 195.3125 uV
            def j_rail(o, rail=rail):
                raw = int.from_bytes(_bytes(o, 3), "big") >> 4
                return _band(round(raw * 195.3125e-6, 3), rail["volts"], "V")
            cmd = lt.xfer_cmd(bus, a, b"\x05", 3, force=True) + " 2>&1"
        out.append(Check(f"rail_{rail['name']}", f"the carrier's {rail['name']} rail monitor reads the rail in band",
                         cmd, j_rail, fixture=gate, tools=("i2ctransfer",)))

    codecs = [(int(str(c["i2c_address"]), 16), _linux_bus(ctx, c["i2c_bus"]) if ctx.bench is not None else 0)
              for c in (board.get("audio") or {}).get("codecs") or []]
    card = cx.get("audio_card")
    if codecs and card:
        def j_audio(o):
            kv = _kv(o)
            if "card=absent" in o:
                raise Fail(f"no ALSA card {card}")
            if int(kv.get("bound") or 0) != len(codecs):
                raise Fail(f"{kv.get('bound') or 0} of {len(codecs)} amplifiers bound to their driver")
            if kv.get("aplay_rc") != "0":
                raise Fail(f"aplay rc={kv.get('aplay_rc')}")
            return _band(float(kv.get("elapsed") or "nan"), _want(cx, "audio_elapsed_s"), "s for a 2 s playback")
        bound = "; ".join(f"[ -e /sys/bus/i2c/devices/{b}-{a:04x}/driver ] && n=$((n+1))" for a, b in codecs)
        out.append(Check("audio", "both amplifiers are bound and a 2 s playback of silence runs in real time",
                         f'C={q(str(card))}; n=0; {bound}; echo "bound=$n"\n' + _HIL_AUDIO,
                         j_audio, timeout_s=10, est_s=2.3, lane="audio", tools=("aplay", "awk", "cut")))

    def j_dsi(o):
        rows = [ln.split() for ln in o.splitlines() if len(ln.split()) == 2]
        if not rows:
            raise Fail("no DSI connector registered")
        return " ".join(f"{n.split('-', 1)[-1]}={s}" for n, s in rows)
    out.append(Check("display_dsi", "the display pipeline registered a DSI connector (a picture needs an operator)",
                     'for c in /sys/class/drm/card*-DSI-*; do [ -e $c/status ] && echo "${c##*/} $(cat $c/status)"; '
                     "done", j_dsi))

    cam = fx.get("camera")

    def j_cam(o):
        pat = cam if isinstance(cam, str) else "."
        hits = [ln for ln in o.splitlines() if ln.strip() and re.search(pat, ln)]
        if not hits:
            raise Fail(f"no video device matches /{pat}/: {' | '.join(o.split())[:80] or 'none registered'}")
        return hits[0].strip()[:40]
    out.append(Check("camera", "the fitted camera's sensor probed (its ID was read by its driver)",
                     "cat /sys/class/video4linux/*/name 2>/dev/null", j_cam, fixture="camera"))

    def j_loop(o):
        if _kv(o).get("rc") != "0":
            raise Fail(f"loopback failed: {' '.join(o.split())[-100:]!r}")
        return "loopback ok"
    out.append(Check(
        "can_loopback", "a frame sent on CAN0 arrives on CAN1 (both transceivers, wired together)",
        _CAN, j_loop, timeout_s=10, est_s=1.0, lane="loop", fixture="can_loopback", files={"can.py": _CAN_PY},
        tools=("ip", "python3", "sed", "head"), setup=_CAN_SETUP, restore=_CAN_RESTORE))
    tty = fx.get("uart_loopback")
    out.append(Check(
        "uart_loopback", "bytes sent on the header UART come back (TX wired to RX)",
        f'T={q(tty) if isinstance(tty, str) else q("")}; [ -c "$T" ] || {{ echo "ALPUNREAD no tty $T"; exit 0; }}\n'
        'python3 "$D/uart.py" "$T" 2>&1; echo "rc=$?"',
        j_loop, timeout_s=10, est_s=0.5, lane="loop", fixture="uart_loopback", files={"uart.py": _UART_PY},
        tools=("python3",)))

    # defence in depth: whatever built the list, nothing may address a shared address on this bus
    for c in out:
        for a, chip in shared.items():
            if re.search(rf"-y {bus} [^\n;|]*@{a:#04x}\b", c.cmd):
                raise ValueError(f"{c.name}: refusing an I2C transfer to {a:#04x} on bus {bus}, "
                                 f"the {chip}'s shared / broadcast address")
    return out


# --------------------------------------------------------------------------
# script, run, judge
# --------------------------------------------------------------------------

def applicable(checks: list[Check], fixtures: dict) -> list[Check]:
    """The checks that run on the unit: every fixture-free one and those whose fixture is on."""
    return [c for c in checks if c.cmd and (c.fixture is None or fixtures.get(c.fixture))]


def informational(checks: list[Check], x: dict) -> set[str]:
    """Names of the checks that never fail the step: the expected-values list, minus any check
    the catalogue forces blocking (e.g. cm33_firmware when the bundle carries a CM33 image)."""
    return set(x.get("informational") or []) - {c.name for c in checks if c.blocking}


def estimate(checks: list[Check]) -> tuple[dict[str, float], float]:
    """ESTIMATED seconds per lane and the wall time (lanes run concurrently)."""
    lanes: dict[str, float] = {}
    for c in checks:
        lanes[c.lane] = lanes.get(c.lane, 0.0) + c.est_s
    return lanes, max(lanes.values(), default=0.0)


def fragment(c: Check) -> str:
    """One check's own script: the missing-tool guard, the restore trap, the commands."""
    lines = [f"# {c.name}: {c.what}"]
    if c.tools:
        lines.append(f"for t in {' '.join(c.tools)}; do command -v \"$t\" >/dev/null 2>&1 || "
                     '{ echo "ALPUNREAD missing tool: $t"; exit 0; }; done')
    if c.restore:
        # The state is recorded in this shell, the commands run in a background subshell and this
        # shell only waits: `wait` is interruptible, so a TERM runs the trap at once (a shell that
        # runs a foreground command would only see it when that command ends). EXIT covers the
        # normal end and, through the TERM trap, the kill.
        lines += [c.setup or ":", "restore() {", c.restore, "}", "trap restore EXIT",
                  "trap 'kill \"$body\" 2>/dev/null; exit 143' TERM INT HUP",
                  "(", c.cmd, ") &", "body=$!", 'wait "$body"', "exit $?"]
    else:
        lines.append(c.cmd)
    return "\n".join(lines)


def script(checks: list[Check]) -> str:
    """The one shell script (POSIX sh, busybox-safe). It works in a fresh temp directory (and
    cd's into it first, so no tool drops a file in the login directory on the root filesystem)
    and removes it on any exit. Every check is a file of its own, run in its own session with
    stdout+stderr in its own file; a check that outlives its timeout gets TERM (its restore
    trap runs), then the whole process group is killed; results are printed at the end,
    framed, in catalogue order."""
    lanes: dict[str, list[Check]] = {}
    for c in checks:
        lanes.setdefault(c.lane, []).append(c)
    files = {n: body for c in checks for n, body in c.files.items()}
    files.update({f"{c.name}.sh": fragment(c) for c in checks})
    out = ["#!/bin/sh", "# generated by scripts/provision/functest.py: the provisioning functional test",
           "D=$(mktemp -d /tmp/alp-ft.XXXXXX) || exit 1",
           "trap 'rm -rf \"$D\"' EXIT", "trap 'exit 1' INT TERM HUP",
           'cd "$D" || exit 1', _EMMC, _NETS, "export D E NETS"]
    for n, body in sorted(files.items()):
        out += [f"cat > \"$D/{n}\" <<'ALPFTEOF'", body.strip("\n"), "ALPFTEOF"]
    out += ["SETSID=; command -v setsid >/dev/null 2>&1 && SETSID=setsid",
            # signal $2's process group, else $2 alone. dash's kill rejects `kill -TERM -- -PID`
            # ("Illegal number") but takes `kill -s TERM -- -PID`; the second form is the
            # other shells' spelling. Tried in order; the last one reaches the leader only.
            'gkill() { kill -s "$1" -- "-$2" 2>/dev/null || kill "-$1" "-$2" 2>/dev/null || kill -s "$1" "$2" 2>/dev/null; }',
            "run() {",
            '  $SETSID sh "$D/$1.sh" >"$D/$1.out" 2>&1 </dev/null &',
            "  p=$!",
            # TERM first so the fragment's restore trap runs, then KILL; the group when the check
            # leads one (setsid), else the process
            '  ( sleep "$2"; : >"$D/$1.to"; gkill TERM "$p"; sleep 3; gkill KILL "$p" ) >/dev/null 2>&1 </dev/null &',
            "  w=$!",
            '  wait "$p"; echo $? >"$D/$1.rc"',
            '  kill "$w" 2>/dev/null; wait "$w" 2>/dev/null',
            # the fragment's shell is gone: reap whatever it left running (dd, aplay, a scan)
            '  [ -e "$D/$1.to" ] && gkill KILL "$p"',
            "  return 0",
            "}"]
    for lane, cs in lanes.items():
        out.append(f"lane_{lane}() {{ " + " ".join(f"run {c.name} {c.timeout_s};" for c in cs) + " }")
    out += [f"lane_{lane} &" for lane in lanes if lane != "main"]
    if "main" in lanes:
        out.append("lane_main")
    out += ["wait",
            f"for n in {' '.join(c.name for c in checks)}; do",
            '  if [ -e "$D/$n.to" ]; then rc=T; else rc=$(cat "$D/$n.rc" 2>/dev/null); fi',
            '  echo "@@ALPFT $n ${rc:-X}"; cat "$D/$n.out" 2>/dev/null; echo; echo "@@ALPFT-END $n"',
            "done", ""]
    return "\n".join(out)


def parse(stdout: str) -> dict[str, tuple[str, str]]:
    """{check name: (rc, output)} from the framed script output."""
    return {m[1]: (m[2], m[3].strip("\n"))
            for m in re.finditer(r"(?ms)^@@ALPFT (\S+) (\S+)\n(.*?)\n@@ALPFT-END \1$", stdout.replace("\r\n", "\n"))}


def _value(kind: str, why) -> str:
    why = " ".join(str(why).split())[:REASON_MAX]
    return lt.unread(why) if kind == "unread" else f"{kind} ({why})"


def judge(c: Check, got: tuple[str, str] | None, info: bool = False) -> str:
    """One check's ledger value. ``info``: the check is informational (decides what a missing
    expected value becomes)."""
    try:
        if c.host is not None:
            measured = c.host()
        else:
            if got is None:
                raise Unread("no result: the script did not get to this check")
            rc, text = got
            if rc == "T":
                raise Unread(f"timed out after {c.timeout_s} s")
            if m := re.search(r"^ALP(UNREAD|SKIP)\b[ \t]*(.*)$", text, re.M):
                raise (Unread if m[1] == "UNREAD" else Skip)(m[2].strip() or "no reason given")
            last = text.strip().splitlines()[-1] if text.strip() else ""
            # by the exit status or the LAST line only: a "not found" somewhere in a kernel log
            # or a tool's own output is not a missing tool
            if rc == "127" or re.search(r": (command )?not found$", last):
                raise Unread(f"missing tool: {last[-80:] or 'exit status 127'}")
            measured = c.judge(text)
    except Fail as e:
        return _value("fail", e)
    except Skip as e:
        return _value("skipped", e)
    except NoExpect as e:
        # a criterion nobody defined: a blocking check has no verdict, which blocks
        return _value("skipped" if info else "unread", f"no expected value: {e}")
    except (Unread, BenchError) as e:
        return _value("unread", e)
    except (ValueError, KeyError, IndexError, TypeError, StopIteration) as e:
        # output in a shape the judge did not expect: not evaluated, never a pass and never a crash
        return _value("unread", f"unparsable output ({type(e).__name__}: {e})")
    measured = " ".join(str(measured or "").split())[:REASON_MAX]
    return f"pass ({measured})" if measured else "pass"


def run(ctx, t, checks: list[Check], x: dict) -> tuple[dict[str, str], str, float]:
    """Run the catalogue on target ``t``. Returns ({check name: value}, log text, seconds).
    The supply is read first, while the unit is idle."""
    import tempfile

    fixtures = config(ctx).get("fixtures") or {}
    info = informational(checks, x)
    values: dict[str, str] = {}
    start = time.monotonic()
    for c in checks:
        if c.host is not None:
            values[c.name] = judge(c, None, c.name in info)
    on_unit = applicable(checks, fixtures)
    got: dict[str, tuple[str, str]] = {}
    raw = ""
    if on_unit:
        lanes: dict[str, int] = {}
        for c in on_unit:
            lanes[c.lane] = lanes.get(c.lane, 0) + c.timeout_s
        with tempfile.TemporaryDirectory(prefix="functest_") as td:
            local = Path(td) / "alp-functest.sh"
            local.write_bytes(script(on_unit).encode("utf-8"))
            t.put(local, REMOTE_SCRIPT)
        try:
            raw = t.run(f"sh {REMOTE_SCRIPT}", timeout=max(lanes.values()) + SCRIPT_MARGIN_S, check=False).stdout
        finally:
            t.run(f"rm -f {REMOTE_SCRIPT}", check=False)
        got = parse(raw)
    by_name = {c.name: c for c in checks}
    for c in checks:
        if c.host is not None:
            continue
        if c.fixture is not None and not fixtures.get(c.fixture):
            values[c.name] = _value("skipped", f"no fixture: {c.fixture}")
        else:
            src = by_name[c.same_as] if c.same_as else c
            values[c.name] = judge(c, got.get(src.name), c.name in info)
    return values, raw, time.monotonic() - start


def blocking(values: dict[str, str], checks: list[Check], x: dict) -> list[str]:
    """Names of the checks that fail the step: fail or unread, and not informational."""
    info = informational(checks, x)
    return [n for n, v in values.items() if n not in info and v.startswith(("fail", "unread"))]


def rtc_set(t) -> str:
    """Set rtc0 from the HOST's UTC clock (the unit's own clock may be unset) and return the
    unit's boot id. Writes the RTC's time registers only; used by cold_boot_test when the
    ``rtc_backup`` fixture is on, so the functional test can prove retention."""
    t.run(f"python3 -c {shlex.quote(_RTC_SET_PY)} {int(time.time())}")
    return t.run("cat /proc/sys/kernel/random/boot_id").stdout.strip()
