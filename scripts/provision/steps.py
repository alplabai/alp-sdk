# SPDX-License-Identifier: Apache-2.0
"""The V2N provisioning step machine.

Every step has a read-only ``probe(ctx)`` and a ``run(ctx)``; every mutation
in ``run`` goes through ``ctx.mutate()``, which in dry run only records what
it would do. ``run_steps`` walks ``STEP_ORDER``: probe -> skip if Satisfied,
else run, then re-probe; a step is ``done`` only when the re-probe is
Satisfied (or, for steps whose outcome cannot be observed until the operator
acts, when run() completed and the probe is still Unknown -- ``trust_run``).

Read-only checks (probes, census, write preconditions, the GD32 DP-ID gate,
the PMIC register compare) run in dry run too whenever a Linux target or
probe is available, so ``plan`` shows real refusals, not just intentions.

See docs/provisioning-v2n.md.
"""

from __future__ import annotations

import hashlib
import json
import os
import math
import re
import secrets
import shlex
import subprocess
import sys
import tempfile
import time
import zlib
from collections.abc import Callable

import yaml
from dataclasses import dataclass, field
from datetime import date, datetime, timezone
from pathlib import Path
from typing import TypeVar

from provision import bmap, dxm1, functest, gates, ledger_out, payload_store, uboot
from provision import linux_target as lt
from provision.bench import Bench, BenchError, ExpectTimeout
from provision.console_target import ConsoleTarget
from provision.store_swd_probe import StoreSwdProbe

T = TypeVar("T")
SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent

GD32_DP_OK = 0x0BE12477
GD32_DP_REFUSE = {0x6BA02477: "the SoC's own debug port (wrong target)",
                  0x4C013477: "a known non-GD32 debug port"}
# --gd32-fw DIR: file -> (load address, ledger key)
GD32_IMAGES = (("bootloader.bin", 0x08000000, "gd32_bootloader_md5"),
               ("ota-meta.bin", 0x08008000, "gd32_ota_meta_md5"),
               ("slot-a.bin", 0x0800A000, "gd32_slot_a_md5"))
GD32_BRIDGE_ADDR = 0x70
SYS_LSI_MODE_XSPI = "0x3c06"
LOGIN_RE = r"login: *$"
IP_WAIT_S = 120.0     # boot_sd_linux: how long a console login may wait for DHCP
IP_POLL_S = 5.0
LOCK_BIT = 0x02
PSU_CURRENT_WARN_A = 0.40   # census screen for the PHY-regulator fault; warns only, never fails
# After a reset-and-run the GD32 needs its bootloader + clock-up before it answers GET_VERSION.
# ponytail: 10 tries x 1 s, taken from the old fixed 5 s settle; no bench timing behind it yet.
BRIDGE_TRIES = 10
BRIDGE_GAP_S = 1.0
I2C_WEDGE_DMESG = "SCL is stuck low"


# --------------------------------------------------------------------------
# probe results / step results
# --------------------------------------------------------------------------

@dataclass
class Satisfied:
    evidence: dict[str, str] = field(default_factory=dict)
    reason: str = ""


@dataclass
class Unsatisfied:
    reason: str


@dataclass
class Unknown:
    reason: str


ProbeResult = Satisfied | Unsatisfied | Unknown


@dataclass
class StepResult:
    name: str
    status: str               # "done" | "skipped" | "failed" | "planned"
    detail: str
    evidence: dict[str, str] = field(default_factory=dict)
    commands: list[str] = field(default_factory=list)


class Refused(Exception):
    """A precondition or gate refused the step (not a transport failure)."""


# --------------------------------------------------------------------------
# context
# --------------------------------------------------------------------------

@dataclass
class Ctx:
    sku: str
    serial: str
    bundle_dir: Path
    bundle: dict
    preset: dict
    ledger_root: Path
    execute: bool = False
    lock: bool = False
    bench: Bench | None = None
    linux: object | None = None           # LinuxTarget (or a duck-typed fake)
    tier_markers: dict | None = None
    expected_registers: dict | None = None
    functest_expect: dict | None = None   # functional_test pass criteria (None: the public defaults)
    allow_tier_mismatch: str | None = None
    accept_cid_change: str | None = None  # operator escape: the eMMC was legitimately replaced
    reprovision_from: Path | None = None
    cold_cycles: int = 3
    hil_spec: Path | None = None
    facts: dict[str, str] = field(default_factory=dict)
    plan_log: list[str] = field(default_factory=list)
    state: dict = field(default_factory=dict)
    # extensions beyond the contract shape
    mfg_date_override: date | None = None
    flash_writer: Path | None = None
    gd32_fw: Path | None = None
    transfer: str = "sd"                  # "sd" | "xmodem"
    # cache payloads in /var/lib/alp-payload on the provisioning SD's root (execute only);
    # off by default so a bare Ctx never writes to the board
    payload_store_on: bool = False
    station: str | None = None
    by: str = "provision_som"
    ledger_xlsx: Path | None = None
    boot_text: str = ""                   # last console capture after a power cycle
    boot_class: str = ""
    # Power.on_count when detect left the unit live at the SCIF ROM prompt
    # (banner consumed); valid only while no later ON has happened.
    rom_live_on_count: int | None = None
    # (Power.on_count, console transcript mark) of the boot dsw1_emmc_insert_sd
    # started and left running; boot_sd_linux continues it instead of re-cycling.
    live_boot: tuple[int, int] | None = None
    # Set once eeprom_manifest changed the MAC: the bench.yaml pinned host (CID-MAC
    # era) is stale from then on, so every later attach rediscovers over the console.
    rediscover_host: bool = False
    # Power.on_count of a boot_sd_linux that reached a console login with no IP
    # (GD32 flash pending): its probe accepts that state; gd32_flash then works over the console.
    console_linux_on_count: int | None = None
    # Power.on_count of the boot this tool last logged in on over the console: Linux may be
    # running with no SSH, and clean_shutdown() then halts it through the console shell.
    console_login_on_count: int | None = None
    # Power.on_count at which clean_shutdown() already halted the unit (once per power-on);
    # at which the unit is known to sit in the SCIF ROM / Flash Writer (nothing but its own
    # CR-terminated lines may be sent there).
    halted_on_count: int | None = None
    halted_outcome: str = ""             # clean_shutdown's outcome for that power-on
    rom_console_on_count: int | None = None
    # one line per power cut (clean_shutdown outcome); run_one copies them into the step evidence
    power_cuts: list[str] = field(default_factory=list)
    step_logs: dict[str, str] = field(default_factory=dict)
    _cache: dict = field(default_factory=dict)

    def mutate(self, description: str, action: Callable[[], T]) -> T | None:
        if not self.execute:
            self.plan_log.append("WOULD: " + description)
            return None
        self.plan_log.append(description)
        return action()

    @property
    def mfg_date(self) -> date:
        return self.mfg_date_override or gates.mfg_date_for_serial(self.serial)

    @property
    def state_path(self) -> Path:
        return self.ledger_root / self.sku / f"{self.serial}.state.json"

    @property
    def unit_dir(self) -> Path:
        return self.ledger_root / self.sku

    @property
    def family(self) -> str:
        return self.bundle.get("family", "")

    # -- artefacts ------------------------------------------------------------
    def artefact(self, role: str) -> Path:
        for c in self.bundle.get("components", []):
            if c.get("role") == role:
                return self.bundle_dir / c["file"]
        raise Refused(f"bundle has no {role} component")

    def artefact_bytes(self, role: str) -> bytes:
        key = ("bytes", role)
        if key not in self._cache:
            self._cache[key] = self.artefact(role).read_bytes()
        return self._cache[key]

    def open_payload_store(self, t) -> payload_store.PayloadStore | None:
        """The SD payload store on target ``t`` (execute only; never in a plan)."""
        if not (self.payload_store_on and self.execute and t is not None):
            return None
        sha = self.state.get("bundle_sha256") or hashlib.sha256(
            json.dumps(self.bundle, sort_keys=True).encode()).hexdigest()
        try:
            root, emmc = lt.root_device(t), lt.resolve_emmc(t)
        except BenchError as e:
            return payload_store.PayloadStore(t, None, off=f"store off: cannot tell the boot device ({e})")
        return payload_store.open_store(t, sha, self.bundle.get("components", []), root, emmc)

    # -- bench ----------------------------------------------------------------
    def need_bench(self) -> Bench:
        if self.bench is None:
            raise Refused("no --bench")
        return self.bench

    def attach_linux(self, host: str) -> None:
        """Attach the Linux target at ``host`` and hand the host to the bench's probe
        wrapper (it reads ALP_PROVISION_HOST; the bench.yaml says to set it)."""
        b = self.need_bench()
        self.linux = lt.LinuxTarget(host, b.linux_user)
        if getattr(b.probe, "env", None) is not None:
            b.probe.env["ALP_PROVISION_HOST"] = host

    @property
    def pinned_host(self) -> str | None:
        """bench.yaml linux.host, unless the MAC changed since (then None: discover)."""
        return None if self.rediscover_host or self.bench is None else self.bench.linux_host

    def need_linux(self):
        """The Linux target, or None in a dry run without one (plan only).
        Like linux_up(), attach the bench's configured host when an --only /
        --from / --force-step run starts past the step that normally
        attaches it, instead of refusing a board that is already up."""
        if self.linux is None and self.pinned_host:
            self.attach_linux(self.pinned_host)
        if self.linux is None and self.execute and self.bench is not None \
                and self.console_linux_on_count == self.bench.power.on_count:
            # boot_sd_linux logged in on this boot without an IP: DHCP may have answered since
            try:
                connect_linux(self, force=True)
            except BenchError:
                self.linux = None
        if self.linux is None and self.execute:
            if self.bench is not None and self.console_linux_on_count == self.bench.power.on_count:
                raise Refused("Linux is up on the console but has no reachable IPv4 host (no DHCP lease on "
                              "end0 and no bench.yaml linux.host that answers); check cable/DHCP, then re-run")
            raise Refused("no Linux target attached: bench.yaml linux.host is unset or unreachable and this "
                          "run has no console login to discover a host from; run boot_sd_linux (or set linux.host)")
        if self.linux is not None and self.execute:
            self._check_unit_identity(self.linux)
        return self.linux

    def recorded_cid(self) -> str:
        """The eMMC CID this serial is known by ("" before first contact). The CID is
        hardware, so it outlives a bundle / tool_rev change: finished steps, then steps
        moved to ``superseded``, then the committed unit.yaml. An explicit
        ``--accept-cid-change`` anchor (``cid_anchor``) outranks all of them."""
        if self.state.get("cid_anchor"):
            return self.state["cid_anchor"]
        groups = [self.state.get("steps", {})] + [g.get("steps", {}) for g in self.state.get("superseded", [])]
        for steps_ in groups:
            for st in steps_.values():
                if st.get("status") in ("done", "skipped") and (st.get("evidence") or {}).get("emmc_cid_raw"):
                    return st["evidence"]["emmc_cid_raw"]
        return ledger_out.read_unit_yaml(self.unit_dir / f"{self.serial}.unit.yaml").get("emmc_cid_raw", "")

    def _check_unit_identity(self, t) -> None:
        """Refuse to mutate a Linux target whose eMMC is not this serial's (stale IP).
        Read fresh on every call, never cached: a power cycle or DHCP renewal can put
        another unit behind the same address. First contact with nothing recorded is
        unchecked (bootstrap's EM_DCID records the CID on the normal blank-unit path)."""
        want = self.recorded_cid()
        if not want and not self.accept_cid_change:
            return
        seen = lt.read_emmc_cid(t)
        if want and lt.cid_identity(seen) == lt.cid_identity(want):
            return
        if self.accept_cid_change:
            if want:                          # first contact adopts silently; a changed CID is an override
                _record_override(self, "emmc_cid_change", self.accept_cid_change)
            self.state["cid_anchor"] = seen.strip().lower()
            save_state(self.state_path, self.state)
            self.accept_cid_change = None   # one adoption per run: a later swap is refused
            return
        raise Refused(f"{t.host} answers with eMMC CID {seen.strip().lower()}, but unit {self.serial} "
                      f"recorded {want.strip().lower()}: another unit is behind this address "
                      "(stale DHCP lease?). Fix the address in bench.yaml and re-run, or "
                      "--accept-cid-change REASON if the eMMC was replaced.")

    def linux_up(self) -> bool:
        # --only/--from can start past detect, which is what normally attaches
        # ctx.linux; attach a configured host here so a probe does not report
        # Unknown and trigger a needless cold cycle on a board already up.
        if self.linux is None and self.pinned_host:
            self.attach_linux(self.pinned_host)
        if self.linux is None:
            return False
        try:
            return self.linux.run("true", timeout=15.0, check=False).rc == 0
        except BenchError:
            return False

    def state_done(self, name: str) -> bool:
        return self.state.get("steps", {}).get(name, {}).get("status") == "done"

    def i2c(self, key: str) -> int:
        bus = self.need_bench().i2c_bus.get(key) if self.bench else None
        if bus is None:
            if key == "eeprom":
                return 0
            raise Refused(f"bench.yaml i2c_bus.{key} is TBD (null)")
        return bus


def _now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def _crc(data: bytes) -> str:
    return f"0x{zlib.crc32(data):08x}"


def expected_family(preset: dict) -> str:
    return "v2n-m1" if str(preset.get("family", "")).endswith("-deepx") else "v2n"


def tier_gate(ctx: Ctx, uboot_mib: int | None = None) -> tuple[gates.GateResult, dict[str, str]]:
    """DDR tier triangle (gates.tier_triangle) over the bundle's BL2 images."""
    m = ctx.tier_markers
    if not m:
        return gates.GateResult("tier_triangle", False, "no --tier-markers given"), {}
    images = [ctx.artefact_bytes(r) for r in ("bl2", "bl2_mmc")
              if any(c.get("role") == r for c in ctx.bundle.get("components", []))]
    r = gates.tier_triangle(images, m, ctx.sku, (ctx.preset.get("memory") or {}).get("dram_mbit"),
                            ctx.bundle.get("memory_tier"), uboot_mib, ctx.allow_tier_mismatch)
    ev = {"dram_tier_check": ("overridden" if r.overridden else "ok" if r.ok else "FAIL") + f": {r.detail}"}
    try:
        ev["bl2_ddr_config"] = " ".join(sorted({gates.scan_tier(img, m) for img in images}))
    except (ValueError, KeyError):
        pass
    return r, ev


def _record_override(ctx: Ctx, gate: str, reason: str) -> None:
    ov = ctx.state.setdefault("overrides", [])
    if not any(o.get("gate") == gate and o.get("reason") == reason for o in ov):
        ov.append({"gate": gate, "reason": reason, "at": _now()})
    # the ledger key: ledger_out.ship_check blocks a unit that carries one
    ctx.facts["provision_overrides"] = "; ".join(f"{o['gate']}: {o['reason']}" for o in ov)


# --------------------------------------------------------------------------
# console / Linux helpers
# --------------------------------------------------------------------------

def _elide_long_lines(text: str, limit: int = 200) -> str:
    """Collapse runs of very long lines (the gd32_flash base64 push echo) to
    head + tail + a byte count, so a step log stays readable."""
    out, run = [], []

    def flush():
        if len(run) > 2:
            out.append(run[0][:60] + "...")
            out.append(f"[{len(run) - 2} long lines, {sum(len(x) for x in run[1:-1])} bytes elided]")
            out.append("..." + run[-1][-20:])
        else:
            out.extend(x[:60] + f"...[{len(x)} bytes]" for x in run)
        run.clear()
    for line in text.split("\n"):
        if len(line) > limit:
            run.append(line)
        else:
            flush()
            out.append(line)
    flush()
    return "\n".join(out)


def _since(console, n: int) -> str:
    return "".join(console.transcript[n:])


def _wait_for_ip(ctx: Ctx, wait_s: float, rediscover: bool) -> None:
    """Poll for an IPv4 host (pinned bench.yaml linux.host, else the console's
    `ip -4 addr` view) until it answers or `wait_s` ran out; ctx.linux stays None then."""
    deadline = time.monotonic() + wait_s
    while (rest := deadline - time.monotonic()) > 0:
        time.sleep(min(IP_POLL_S, rest))
        try:
            connect_linux(ctx, force=True, rediscover=rediscover)
            ctx.linux.run("true")
            return
        except BenchError:
            ctx.linux = None


HALT_RE = r"reboot: Power down|System halted"
HALT_WAIT_S = 120.0          # the halt line after `poweroff`; a microSD root can hold seconds of writeback
HALT_LATE_S = 5.0            # one more look before the cut is called a fallback
POWEROFF_TIMEOUT_S = 120.0   # `sync; poweroff` itself
PROBE_S = 2.0                # Ctrl-C probe of a console whose state is unknown
ID_WAIT_S = 5.0              # the SSH identity nonce on this unit's console
PROBE_UNKNOWN_CONSOLE = True
DETECT_WAIT_S = 240.0


def console_login_ctx(ctx: Ctx, timeout: float | None = None) -> None:
    """lt.console_login on the bench console, remembering which power-on it logged in on
    (clean_shutdown() then halts that boot through the console shell)."""
    b = ctx.need_bench()
    if timeout is None:
        lt.console_login(b.console, b.linux_user)
    else:
        lt.console_login(b.console, b.linux_user, timeout=timeout)
    ctx.console_login_on_count = b.power.on_count


def _nonce() -> str:
    return secrets.token_hex(4)


def _record_cut(ctx: Ctx, outcome: str, why: str) -> str:
    """Every power cut, durably: ``ctx.power_cuts`` (run_one copies it into the step's
    ``power_cut`` evidence and its log) and the plan log."""
    line = f"{outcome}: {why}"
    ctx.power_cuts.append(line)
    ctx.plan_log.append("power cut " + line)
    return outcome


def halt_note(outcome: str) -> str:
    """The operator-prompt sentence for a clean_shutdown() outcome."""
    outcome = outcome or ""
    if outcome == "clean":
        return "The unit has been halted (poweroff, halt line seen)."
    if outcome.startswith("clean ("):
        return (f"The unit was halted (poweroff, halt line seen), but the final sync reported an error "
                f"({outcome[7:-1]}); check the card before reusing it.")
    if outcome == "not-needed":
        return "No running Linux was found, so nothing was halted."
    return (f"WARNING: the unit was NOT cleanly halted ({outcome or 'no shutdown attempted'}); "
            "a mounted microSD root may be dirty.")


def _same_unit(ctx: Ctx, t) -> bool:
    """Prove that the SSH host is the unit on THIS console: it must write a nonce to its
    /dev/console and we must read it on our serial line (a stale DHCP lease may point at
    another unit, which `poweroff` would then halt instead)."""
    b = ctx.bench
    nonce = _nonce()
    b.console.drain()
    try:
        t.run(f"echo ALPID{nonce} > /dev/console", check=False, timeout=15.0)
        b.console.expect(f"ALPID{nonce}", ID_WAIT_S)
    except BenchError:
        return False
    return True


def _probe_console(ctx: Ctx):
    """A console whose state is unknown. Send Ctrl-C ONLY and read PROBE_S: a U-Boot prompt at
    the end of the text means there is no Linux ("uboot"). No Enter is sent until a U-Boot
    prompt has been ruled out (U-Boot repeats its last command on one); only then a console
    login (5 s), which starts with an Enter. Returns a ConsoleTarget on a shell, else None.
    Not sent to a unit known to be in the SCIF ROM / Flash Writer (clean_shutdown returns before):
    this tool's parsers send those only CR-terminated lines and never 0x03."""
    b = ctx.bench
    b.console.drain()
    b.console.write(b"\x03")
    b.console.pump(PROBE_S)
    if re.search(rf"(?:^|\n){uboot.PROMPT}\Z", b.console.peek()):      # a prompt, not a `=> ` in a log line
        return "uboot"
    try:
        console_login_ctx(ctx, timeout=5.0)
    except BenchError:
        return None
    return ConsoleTarget(b.console)


def clean_shutdown(ctx: Ctx) -> str:
    """Halt a running Linux before ANY tool-driven power cut (a hard cut under a mounted
    microSD root corrupted its journal, #2624). Execute-mode only. Returns and records
    (``ctx.power_cuts`` -> the step's ``power_cut`` evidence) the outcome:

    - ``clean``: `poweroff`, then the kernel's halt line;
    - ``fallback``: no halt line in HALT_WAIT_S + HALT_LATE_S; says what the syncs did;
    - ``blind``: no way to reach the unit (no console shell, no SSH proven to be this unit);
      if Linux is up the cut is hard;
    - ``not-needed``: PSU off, U-Boot, SCIF ROM / Flash Writer, or already halted on this ON.

    Path: this boot's console login, else SSH to the pinned/discovered host after it has
    echoed a nonce on this unit's console, else a Ctrl-C probe of the console. The PSU stays
    ON; the caller's Power.cycle does the OFF dwell, so a cold boot stays a cold boot. The
    supply current is not a halt signal. A kernel still booting is not covered."""
    b = ctx.bench
    if b is None or not ctx.execute:
        return ""
    count = b.power.on_count
    if b.power.is_on() is False:
        return _record_cut(ctx, "not-needed", "PSU already off")
    if ctx.halted_on_count == count:
        if ctx.halted_outcome == "clean":
            return _record_cut(ctx, "not-needed", "already halted on this power-on")
        # an earlier cut that was not a plain clean one stays what it was
        return _record_cut(ctx, ctx.halted_outcome, f"earlier cut on this power-on was {ctx.halted_outcome}")
    if ctx.rom_console_on_count == count:
        return _record_cut(ctx, "not-needed", "SCIF ROM / Flash Writer live, no Linux; nothing sent")
    t, via, skipped = None, "", ""
    if ctx.console_login_on_count == count:
        t, via = ConsoleTarget(b.console), "console"
    elif ctx.linux_up():
        if _same_unit(ctx, ctx.linux):
            t, via = ctx.linux, "ssh"
        else:
            skipped = (f"ssh host {getattr(ctx.linux, 'host', '?')} did not echo this unit's console nonce "
                       "(another unit / stale lease): no poweroff sent over ssh; ")
    if t is None and PROBE_UNKNOWN_CONSOLE:
        found = _probe_console(ctx)
        if found == "uboot":
            return _record_cut(ctx, "not-needed", "U-Boot prompt, no shutdown needed")
        if found is not None:
            t, via = found, "console"
    if t is None:
        return _record_cut(ctx, "blind", skipped + "no console shell and no proven ssh path; "
                           "if Linux is up this cut is hard")
    b.console.drain()
    if via == "ssh":
        try:
            r = t.run("sync; echo ALPSYNC:$?; poweroff", check=False, timeout=POWEROFF_TIMEOUT_S)
            m = re.search(r"ALPSYNC:(\d+)", r.stdout)
            sync_rc = m[1] if m else "unknown"
        except BenchError as e:                  # ssh may also drop with the host
            sync_rc = f"unknown ({e})"
    else:
        nonce = _nonce()
        # one console line, paced like ConsoleTarget (RX overruns on a burst); the marker is split
        # in the typed line so its echo cannot match
        b.console.send_line(f'sync; echo "ALPS""{nonce}:$?"; poweroff', paced=True)
        try:
            sync_rc = b.console.expect(rf"ALPS{nonce}:(\d+)\r?\n", POWEROFF_TIMEOUT_S).group(1)
        except ExpectTimeout:
            sync_rc = "unknown (no marker; sync may still be running)"
    late = ""
    try:
        b.console.expect(HALT_RE, HALT_WAIT_S)
        halted, second = True, ""
    except ExpectTimeout:
        # never a second command on the console: the first may still be running, and
        # ConsoleTarget would Ctrl-C it, cancelling the poweroff
        if via == "ssh":
            try:
                second = f"second sync rc={t.run('sync', check=False, timeout=30.0).rc}"
            except BenchError as e:
                second = f"second sync raised: {e}"
        else:
            second = "no second command sent on the console"
        try:
            b.console.expect(HALT_RE, HALT_LATE_S)
            halted, late = True, " (late)"
        except ExpectTimeout:
            halted = False
    ctx.halted_on_count, ctx.console_login_on_count = count, None   # once per power-on
    if halted:
        # `clean` means halted AND the first sync returned 0; any other rc is its own outcome
        outcome = "clean" if sync_rc == "0" else f"clean (sync rc={sync_rc})"
        ctx.halted_outcome = outcome
        return _record_cut(ctx, outcome, f"{via} poweroff, halt line seen{late}; sync rc={sync_rc}")
    ctx.halted_outcome = "fallback"
    return _record_cut(ctx, "fallback", f"{via}: no halt line in {HALT_WAIT_S:g}s + {HALT_LATE_S:g}s; "
                       f"first sync rc={sync_rc}; {second}")


def boot_to_linux(ctx: Ctx, timeout: float = 240.0, need_ip: bool = True,
                  resume_mark: int | None = None, rediscover: bool = False,
                  ip_wait_s: float = 0.0) -> str:
    """Cold cycle, let the unit autoboot to a login, log in, (re)discover the
    Linux target. Returns the whole boot text. Execute-mode only.

    ``need_ip=False`` tolerates a unit with no network (e.g. a latched
    PHY, #2582): ctx.linux is then None and the console shell is the only way in.

    ``ip_wait_s`` keeps polling that long for a host before giving up on the network
    (slow DHCP); 0 = one look, as before.

    ``resume_mark`` (a console transcript index) continues a boot that is already
    running instead of cycling: the text is taken from that mark."""
    b = ctx.need_bench()
    if resume_mark is None:
        n = len(b.console.transcript)
        clean_shutdown(ctx)
        b.console.drain()
        b.power.cycle(b.off_s, b.console)
    else:
        n = resume_mark
    b.console.expect(LOGIN_RE, timeout)
    text = _since(b.console, n)
    ctx.boot_text = text
    console_login_ctx(ctx)
    try:
        connect_linux(ctx, force=True, rediscover=rediscover)
        if not need_ip:
            ctx.linux.run("true")
    except BenchError:
        if need_ip:
            raise
        ctx.linux = None
        if ip_wait_s > 0:
            _wait_for_ip(ctx, ip_wait_s, rediscover)
    return text


PHY_LATCH_DMESG = r"Failed to reset the dma|DMA engine initialization failed|Hw setup failed"


def _gd32_flash_pending(ctx: Ctx) -> bool:
    """A GD32 flash is still to run: a unit with no network may then continue on the console
    (gd32_flash and the later steps work over it); otherwise no network is a failure."""
    return ctx.gd32_fw is not None and not ctx.state_done("gd32_flash")


def _phy_latch_evidence(ctx: Ctx) -> str:
    """Why end0 has no network (#2582), read over the console: "gbeth DMA reset failed on
    <ports>" from the stmmac dmesg lines, or "no carrier" (interface present, link down).
    "" when neither is there."""
    sh = ConsoleTarget(ctx.need_bench().console)
    out = sh.run(f"dmesg | grep -E '{PHY_LATCH_DMESG}'", check=False).stdout
    if out.strip():
        ports = sorted(set(re.findall(r"\b(?:end|eth)\d+\b", out)))
        return f"gbeth DMA reset failed on {', '.join(ports) or 'unknown ports'}"
    return "no carrier" if "end0" in lt.net_ifaces(sh) and not lt.net_carrier(sh, "end0") else ""


def _net_after_boot(ctx: Ctx, ev: dict, text: str) -> str:
    """boot_sd_linux's network tail after a console login without a host: on the #2582
    signature ONE extra cold cycle (Power.cycle keeps MIN_OFF_S / MIN_ON_S), otherwise a
    bounded wait for a slow DHCP lease. ctx.linux may still be None on return; the caller
    decides (a pending GD32 flash is the only acceptable reason)."""
    if ctx.linux is not None:
        return text
    if why := _phy_latch_evidence(ctx):
        ctx.plan_log.append(f"end0 has no network ({why}): PHY latch #2582, one extra cold cycle")
        ev["end0_no_carrier_retries"] = "1"
        text = boot_to_linux(ctx, need_ip=False, ip_wait_s=IP_WAIT_S)
        if ctx.linux is None:
            why = _phy_latch_evidence(ctx) or why
            if not _gd32_flash_pending(ctx):
                raise BenchError("Linux up on console but end0 has no network after the extra cold cycle: "
                                 f"alp-sdk #2582 ({why}); check cable, then power-cycle the unit")
            ev["network"] = f"none ({why})"
        return text
    _wait_for_ip(ctx, IP_WAIT_S, False)
    return text


def cold_boot_phy_retry(ctx: Ctx, ev: dict) -> str:
    """One cold boot. A PHY that latched dead (end0 without carrier, #2582) gets
    exactly one extra cold cycle; end0 must then have carrier, because an IP on
    end1 or a static host proves nothing about end0. The retry count goes in the
    step evidence as ``end0_no_carrier_retries`` (not a ledger catalogue key)."""
    text = boot_to_linux(ctx, need_ip=False)
    sh = ctx.linux or ConsoleTarget(ctx.bench.console)
    if "end0" in lt.net_ifaces(sh) and not lt.net_carrier(sh, "end0"):
        ev["end0_no_carrier_retries"] = str(int(ev.get("end0_no_carrier_retries", "0")) + 1)
        text += boot_to_linux(ctx, need_ip=False)
        sh = ctx.linux or ConsoleTarget(ctx.bench.console)
        if not lt.net_carrier(sh, "end0"):
            raise BenchError("end0 still has no carrier after the extra cold cycle (PHY latch, #2582)")
    if ctx.linux is None:
        raise BenchError("no IP after the cold boot although end0 has carrier")
    return text


def connect_linux(ctx: Ctx, force: bool = False, rediscover: bool = False) -> None:
    """Attach ctx.linux: bench.yaml linux.host, else discover over the console.
    ``rediscover`` ignores a pinned host: DHCP may have handed a new address after
    the MAC changed (eeprom_manifest)."""
    if ctx.linux is not None and not force:
        return
    b = ctx.need_bench()
    host = lt.discover_host(b.console) if rediscover or ctx.rediscover_host else (
        b.linux_host or lt.discover_host(b.console))
    if ctx.linux is None or getattr(ctx.linux, "host", None) != host:
        ctx.attach_linux(host)
    if ctx.execute:
        # A new address after a power cycle may belong to another unit: never test it blind.
        ctx._check_unit_identity(ctx.linux)


def _manifest_sku(arr: bytes) -> str:
    return arr[24:48].split(b"\0", 1)[0].decode("ascii", "replace")


def _build_blob(ctx: Ctx, tool: str, name: str) -> bytes:
    """program_eeprom.py / program_eeprom_secure_page.py, host-side, in a temp dir."""
    key = ("blob", tool)
    if key in ctx._cache:
        return ctx._cache[key]
    with tempfile.TemporaryDirectory(prefix="provision_") as td:
        by = Path(td) / "board.yaml"
        # write-text-newline-exempt: tempdir board.yaml, never in the repo tree
        by.write_text(f"som:\n  sku: {ctx.sku}\n  hw_rev: {ctx.bundle['hw_rev']}\n", encoding="utf-8")
        out = Path(td) / name
        proc = subprocess.run(
            [sys.executable, str(SCRIPTS / tool), "--board-yaml", str(by), "--serial", ctx.serial,
             "--mfg-date", ctx.mfg_date.isoformat(), "--output", str(out)],
            capture_output=True, text=True, encoding="utf-8", check=False,
            env={**os.environ, "PYTHONIOENCODING": "utf-8"})
        if proc.returncode != 0:
            raise Refused(f"{tool} failed: {(proc.stderr or proc.stdout).strip()}")
        ctx._cache[key] = out.read_bytes()
    return ctx._cache[key]


def _stage(ctx: Ctx, name: str, data: bytes) -> Path:
    p = ctx.unit_dir / name
    if p.exists() and p.read_bytes() != data:
        raise Refused(f"{p} exists with different content; refusing to replace a staged blob")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p


# --------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------

class Step:
    name = ""
    operator = False
    always_run = False    # probe is never Satisfied; run() decides the outcome
    trust_run = False     # an Unknown re-probe after a clean run() counts as done

    def probe(self, ctx: Ctx) -> ProbeResult:
        return Unknown("no probe")

    def run(self, ctx: Ctx) -> StepResult:
        raise NotImplementedError

    def result(self, ctx: Ctx, detail: str, evidence=None, status: str | None = None) -> StepResult:
        return StepResult(self.name, status or ("done" if ctx.execute else "planned"),
                          detail, dict(evidence or {}))


class Preflight(Step):
    name = "preflight"
    always_run = True

    def run(self, ctx: Ctx) -> StepResult:
        results: list[gates.GateResult] = []
        ev: dict[str, str] = {}
        if (ctx.bundle_dir / "bundle.json").is_file():   # absent for --build-dir
            proc = subprocess.run(
                [sys.executable, str(SCRIPTS / "check_som_bundle.py"), "--bundle",
                 str(ctx.bundle_dir / "bundle.json")],
                capture_output=True, text=True, encoding="utf-8", check=False,
                env={**os.environ, "PYTHONIOENCODING": "utf-8"})
            results.append(gates.GateResult("bundle_schema", proc.returncode == 0,
                                            (proc.stdout or proc.stderr).strip()[-400:]))
        results.append(gates.artefacts(ctx.bundle_dir, ctx.bundle))
        fam = expected_family(ctx.preset)
        results.append(gates.GateResult("family", ctx.family == fam,
                                        f"bundle family {ctx.family!r}, preset implies {fam!r}"))
        results.append(gates.sku_triangle(ctx.sku, ctx.bundle.get("sku", ""),
                                          ctx.preset.get("sku", ""), None))
        try:
            tier, tev = tier_gate(ctx)
        except (Refused, OSError, ValueError, KeyError) as e:
            tier, tev = gates.GateResult("tier_triangle", False, str(e)), {}
        results.append(tier)
        ev.update(tev)
        if tier.overridden:
            _record_override(ctx, "tier_triangle", ctx.allow_tier_mismatch or "")
        try:
            fip = ctx.artefact_bytes("fip")
            rail = gates.fip_rail(fip, ctx.family)
            results.append(rail)
            ev["fip_rail_string"] = "present" if gates.RAIL_PG.encode() in fip else "absent"
            ev["fip_sha256"] = hashlib.sha256(fip).hexdigest()
            try:
                ev["fip_fdtfile"] = gates.fip_fdtfile(fip)
            except ValueError:
                pass
            results.append(gates.fdt(fip, ctx.artefact("system_image")))
        except (Refused, OSError) as e:
            results.append(gates.GateResult("fip", False, str(e)))
        for role, key in (("bl2", "bl2_sha256"), ("system_image", "rootfs_wic_sha256")):
            c = next((c for c in ctx.bundle.get("components", []) if c.get("role") == role), None)
            if c:
                ev[key] = c.get("sha256", "")
        ev["rootfs_bundle_version"] = str(ctx.bundle.get("release_version", ""))
        try:
            for op in gates.IdentityOp:
                gates.identity_frame(op, b"\xff" * gates.SECURE_PAGE_LEN
                                     if op is gates.IdentityOp.SECURE_PAGE_WRITE else b"")
            results.append(gates.GateResult("identity_table", True, "N24S128 frame table self-check ok"))
        except (ValueError, AssertionError) as e:
            results.append(gates.GateResult("identity_table", False, str(e)))
        try:
            n = len(functest.build(ctx))        # pure: bench.yaml functional_test + the expected values
            results.append(gates.GateResult("functional_test_config", True, f"{n} checks"))
        except (ValueError, KeyError, TypeError, OSError, yaml.YAMLError) as e:
            results.append(gates.GateResult("functional_test_config", False, f"{type(e).__name__}: {e}"))
        if ctx.execute:
            # pmic_verify needs it; refuse here, not after every destructive step.
            results.append(gates.GateResult("pmic_expect", bool(ctx.expected_registers),
                                            "expected-registers file given" if ctx.expected_registers
                                            else "run --execute needs --pmic-expect"))
        bad = [r for r in results if not r.ok]
        lines = [f"{'ok ' if r.ok else 'FAIL'} {r.name}: {r.detail}" for r in results]
        ctx.step_logs[self.name] = "\n".join(lines)
        if bad:
            return StepResult(self.name, "failed",
                              "; ".join(f"{r.name}: {r.detail}" for r in bad), ev)
        return StepResult(self.name, "done", f"{len(results)} gates ok", ev)


class Detect(Step):
    name = "detect"
    always_run = True

    def run(self, ctx: Ctx) -> StepResult:
        if ctx.bench is None:
            return self.result(ctx, "no --bench: offline plan", status="skipped")
        c = ctx.bench.console
        from provision import scif_writer
        classes = {"scif-rom": scif_writer.ROM_BANNER, "scif-fallback": scif_writer.ROM_FALLBACK, "linux-login": LOGIN_RE, "uboot": uboot.PROMPT}
        n = len(c.transcript)
        if ctx.execute:
            clean_shutdown(ctx)
            ctx.mutate("power cycle and classify the console",
                       lambda: ctx.bench.power.cycle(ctx.bench.off_s, c))
            try:
                key, _ = c.expect_any(classes, DETECT_WAIT_S)
            except ExpectTimeout:
                key = None
            text = _since(c, n)
        else:
            text = c.drain()
            key = next((k for k, rx in classes.items() if re.search(rx, text, re.MULTILINE)), None)
        if key is None:
            key = "bl2" if uboot.BL2_VERSION_RE.search(text) else "silent"
        ctx.boot_class, ctx.boot_text = key, text
        if key == "scif-fallback":
            raise Refused(scif_writer.ROM_FALLBACK_MSG)
        ctx.rom_live_on_count = ctx.bench.power.on_count if key == "scif-rom" and ctx.execute else None
        if key == "scif-rom" and ctx.execute:
            ctx.rom_console_on_count = ctx.bench.power.on_count
        ev = {**uboot.parse_bl2(text), **uboot.parse_bl31(text)}
        ev.pop("bl2_boot_source", None)
        if v := uboot.parse_uboot_version(text):
            ev["uboot_version"] = v
        if key == "linux-login" and ctx.execute:
            console_login_ctx(ctx)
            connect_linux(ctx, force=True)
        elif ctx.linux is None and ctx.pinned_host:
            ctx.attach_linux(ctx.pinned_host)
        up = ctx.linux_up()
        note = ""
        if key == "silent" and ctx.state_done("bootstrap"):
            # A unit whose eMMC boot partition 1 holds a good bootstrap prints BL2 after a
            # power cycle. Silence means that record is stale (corrupt write,
            # erased part, wrong DSW1): drop it so bootstrap runs again instead
            # of the flow timing out later waiting for a Linux login.
            ctx.state["steps"].pop("bootstrap", None)
            note = "; recorded bootstrap dropped (unit silent after power cycle)"
        return self.result(ctx, f"unit state: {key}; Linux target {'reachable' if up else 'not reachable'}{note}",
                           ev, status="done")


class _PreLinux(Step):
    """Pre-Linux steps are moot once a Linux target answers: everything they
    put in place is rewritten from Linux by the later write_* steps."""
    trust_run = True

    def probe(self, ctx: Ctx) -> ProbeResult:
        if ctx.linux_up():
            return Satisfied({}, "Linux target reachable")
        if ctx.state_done(self.name):
            return Satisfied({}, "state file: done")
        return self.probe_console(ctx)

    def probe_console(self, ctx: Ctx) -> ProbeResult:
        return Unknown("not observable before the next boot")


class OpDsw1Scif(_PreLinux):
    name = "dsw1_scif"
    operator = True

    def probe_console(self, ctx):
        return Satisfied({}, "boot ROM SCIF banner seen") if ctx.boot_class == "scif-rom" \
            else Unknown("boot ROM banner not seen")

    def run(self, ctx):
        b = ctx.need_bench()
        ctx.mutate("operator: set DSW1 to SCIF download mode",
                   lambda: b.operator.confirm("Set DSW1 to SCIF download mode (power stays on)."))
        return self.result(ctx, "DSW1 set to SCIF download (confirmed by bootstrap's ROM banner)")


class Bootstrap(_PreLinux):
    name = "bootstrap"

    def run(self, ctx):
        from provision import scif_writer as sw
        b = ctx.need_bench()
        mot = ctx.flash_writer or b.scif.get("flash_writer")
        ps = b.scif.get("program_start") or {}
        if not mot:
            raise Refused("no Flash Writer: pass --flash-writer or set bench.yaml scif.flash_writer")
        if ps.get("bl2_mmc") is None or ps.get("fip") is None:
            raise Refused("bench.yaml scif.program_start.{bl2_mmc,fip} is TBD (null)")
        bl2 = ctx.artefact_bytes("bl2_mmc")
        fip = ctx.artefact_bytes("fip")
        c = b.console

        def load():
            live = ctx.rom_live_on_count is not None and ctx.rom_live_on_count == b.power.on_count
            ctx.rom_live_on_count = None  # one-shot: a retry must not trust a stale ROM state
            if live:
                # detect just left the unit at the ROM prompt: another cycle would
                # be a second power toggle for nothing (and a PHY power-rule risk)
                ctx.plan_log.append("reuse live SCIF ROM state from detect (no power cycle)")
                sw.load_writer(c, Path(mot), banner_seen=True)
                ctx.rom_console_on_count = b.power.on_count
                return
            clean_shutdown(ctx)
            c.drain()
            b.power.cycle(b.off_s, b.console)
            sw.load_writer(c, Path(mot))
            ctx.rom_console_on_count = b.power.on_count
        ctx.mutate(f"power cycle (unless detect left the ROM live); load Flash Writer {Path(mot).name} over SCIF", load)
        ctx.mutate(f"EM_W area {sw.BOOT1_AREA} sector {sw.BL2_MMC_SECTOR:#x}: bl2_mmc ({len(bl2)} bytes)",
                   lambda: sw.em_w(c, sw.BOOT1_AREA, sw.BL2_MMC_SECTOR, ps["bl2_mmc"], bl2))
        ctx.mutate(f"EM_W area {sw.BOOT1_AREA} sector {sw.FIP_SECTOR:#x}: fip ({len(fip)} bytes)",
                   lambda: sw.em_w(c, sw.BOOT1_AREA, sw.FIP_SECTOR, ps["fip"], fip))
        for idx, val in sw.EXT_CSD_WRITES:
            ctx.mutate(f"EM_SECSD EXT_CSD[{idx}] = {val:#04x}", lambda i=idx, v=val: sw.em_secsd(c, i, v))
        ev = ctx.mutate("EM_DCID (read eMMC CID)", lambda: sw.em_dcid(c)) or {}
        return self.result(ctx, "transient bl2_mmc + fip in eMMC boot partition 1, EXT_CSD 177/179 set", ev)


class OpDsw1EmmcInsertSd(_PreLinux):
    name = "dsw1_emmc_insert_sd"
    operator = True

    def probe_console(self, ctx):
        return Satisfied({}, "U-Boot autoboot seen") if re.search(uboot.AUTOBOOT, ctx.boot_text) \
            else Unknown("U-Boot not seen since the last power cycle")

    def run(self, ctx):
        b = ctx.need_bench()
        def prompt():
            # halt first: the unit runs from the provisioning SD, and the operator is about
            # to switch it off and swap the card
            note = halt_note(clean_shutdown(ctx))
            b.operator.confirm(f"{note} Power OFF, set DSW1 to eMMC boot, insert the release microSD. "
                               "Leave it OFF until the tool asks.")
            ctx.halted_on_count = None     # the operator may have powered it back on: never trust the marker now
        ctx.mutate("clean shutdown; operator: set DSW1 to eMMC boot and insert the release microSD", prompt)

        def check():
            n = len(b.console.transcript)
            b.console.drain()
            b.power.cycle(b.off_s, b.console)
            b.console.expect(uboot.AUTOBOOT, 60.0)
            ctx.boot_text = _since(b.console, n)
            ctx.live_boot = (b.power.on_count, n)
        ctx.mutate("cold cycle; expect U-Boot autoboot from eMMC", check)
        return self.result(ctx, "U-Boot boots from eMMC boot partition 1")


def leftover_dxuart2_swap(ctx) -> str:
    """A refusal message when ``/boot/<fdtfile>.release`` exists on the live unit: a
    dxm1_npu_flash run died after the DTB swap and could not restore it, so the unit boots
    the dxuart2 DTB (PCIe off). "" when clean, not a DX-M1 SKU, or not checkable."""
    if ctx.family != "v2n-m1" or ctx.linux is None:
        return ""
    try:
        bak = f"/boot/{gates.fip_fdtfile(ctx.artefact_bytes('fip'))}.release"
        if ctx.linux.run(f"test -e {shlex.quote(bak)}", check=False).rc != 0:
            return ""
    except (BenchError, Refused, ValueError, KeyError, OSError):
        return ""
    return (f"{bak} exists: an interrupted dxm1_npu_flash left the dxuart2 DTB installed. Copy "
            f"{bak} back over its live DTB and remove it (or run `--only dxm1_npu_flash`, which heals it) "
            "before provisioning further")


class BootSdLinux(Step):
    name = "boot_sd_linux"

    def probe(self, ctx):
        if ctx.linux_up() and leftover_dxuart2_swap(ctx):
            return Unknown("leftover dxuart2 DTB swap on the unit")      # run() refuses with the reason
        if not ctx.linux_up():
            if ctx.bench is not None and ctx.console_linux_on_count == ctx.bench.power.on_count:
                return Satisfied({}, "console login on the live boot, root on the microSD, no IP yet "
                                     "(gd32_flash runs next over the console)")
            return Unknown("no Linux target reachable")
        try:
            root = lt.root_device(ctx.linux)
        except BenchError:
            return Unknown("Linux reachable but its root device is unknown")
        if ctx.transfer == "sd" and root.startswith(lt.resolve_emmc(ctx.linux)):
            return Unsatisfied(f"Linux root {root} is on the eMMC, not the microSD")
        return Satisfied({}, f"Linux target reachable (root {root})")

    def run(self, ctx):
        b = ctx.need_bench()
        if ctx.linux_up() and (why := leftover_dxuart2_swap(ctx)):
            raise Refused(why)
        ev: dict[str, str] = {}
        if ctx.transfer == "xmodem":
            addr = (b.raw.get("uboot") or {}).get("load_addr")
            chunk = int((b.raw.get("uboot") or {}).get("gzwrite_chunk") or 16 << 20)
            if addr is None:
                raise Refused("--transfer xmodem needs bench.yaml uboot.load_addr")
            wic = ctx.artefact("system_image")

            def xm():
                clean_shutdown(ctx)
                text = uboot.cold_to_prompt(b.console, b.power, b.off_s)
                ctx.boot_text = text
                uboot.loadx_gzwrite(b.console, wic, 0, int(addr), chunk)
                n = len(b.console.transcript)
                b.console.send_line("boot")
                b.console.expect(LOGIN_RE, 240.0)
                console_login_ctx(ctx)
                connect_linux(ctx, force=True)
                return text + _since(b.console, n)
            text = ctx.mutate(f"U-Boot loadx + gzwrite {wic.name} into eMMC (slow fallback), boot", xm)
        else:
            def boot():
                live, ctx.live_boot = ctx.live_boot, None   # one-shot
                if live is not None and live[0] == b.power.on_count:
                    # dsw1_emmc_insert_sd just booted the unit: continue that boot
                    ctx.plan_log.append("continue the live boot from dsw1_emmc_insert_sd (no power cycle)")
                    return _net_after_boot(ctx, ev, boot_to_linux(ctx, need_ip=False, resume_mark=live[1]))
                return _net_after_boot(ctx, ev, boot_to_linux(ctx, need_ip=False))
            text = ctx.mutate("cold cycle (unless dsw1_emmc_insert_sd left the unit booting); U-Boot "
                              "bootcmd_check boots the release wic from microSD; "
                              "console login; discover the IPv4 host", boot)
        if text is None:
            return self.result(ctx, f"would boot Linux ({ctx.transfer} path)")
        mib = uboot.parse_dram_banner(text)
        ev.update(uboot.parse_bl2(text))
        ev.update(uboot.parse_bl31(text))
        ev.pop("bl2_boot_source", None)
        if v := uboot.parse_uboot_version(text):
            ev["uboot_version"] = v
        if mib:
            ev["dram_size_mib"] = str(mib)
            ev["uboot_dram_banner"] = next(ln.strip() for ln in text.splitlines() if ln.startswith("DRAM:"))
        tier, tev = tier_gate(ctx, mib)
        ev.update(tev)
        if not tier.ok:
            raise Refused(f"DRAM banner leg: {tier.detail}")
        t = ctx.linux
        if t is None:
            # No IP after the bounded wait (and the extra cold cycle on a PHY latch). A pending
            # GD32 flash lets the unit continue: gd32_flash then works over the console. Any
            # other unit has a cable/DHCP problem: fail, never "done" with no Linux target.
            if not _gd32_flash_pending(ctx):
                raise BenchError(f"Linux up on console but no IPv4 on end0 after {IP_WAIT_S:g}s; "
                                 "check cable/DHCP")
            t = ConsoleTarget(b.console)
            ctx.console_linux_on_count = b.power.on_count
            ev.setdefault("network", f"none (no IPv4 on end0 after {IP_WAIT_S:g}s, no PHY fault seen)")
        else:
            t.run("true")
        if ctx.transfer == "sd":
            root, emmc = lt.root_device(t), lt.resolve_emmc(t)
            if root.startswith(emmc):
                raise Refused(f"Linux root {root} is on the eMMC, not the microSD "
                              "(SDHI1 not up? SD mux? check U-Boot patch 0008)")
        probs = som_presence_problems(ctx, t)
        if probs:
            raise Refused("the SoM on the bench does not match the preset "
                          f"({ctx.sku}): " + "; ".join(probs))
        return self.result(ctx, f"Linux up on {getattr(t, 'host', '?')}" if ctx.linux is not None
                           else "Linux up on the console, no network yet", ev)


def som_presence_problems(ctx, t=None) -> list[str]:
    """The live board check: every non-optional on-module I2C device the
    SoM preset declares must ACK, checked once Linux runs and BEFORE the
    first destructive write. Declared data (bundle, preset, bench.yaml) can
    all agree while the module on the bench is a different SKU; the devices
    answering on the bus cannot. Same set ColdBootTest requires at the end,
    minus the GD32 bridge, which may legitimately be held in reset here."""
    t = t or ctx.linux
    if ctx.bench is None or t is None:
        return []
    expected = {bus: a - {GD32_BRIDGE_ADDR}
                for bus, a in lt.expected_i2c(ctx.preset, ctx.bench.i2c_bus).items()
                if bus is not None}
    return lt.i2c_check(t, expected)


class WriteXspi(Step):
    name = "write_xspi"

    def probe(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        ev = {}
        for key, mtd, role in (("xspi_bl2_md5", 0, "bl2"), ("xspi_fip_md5", 1, "fip")):
            data = ctx.artefact_bytes(role)
            got = t.md5(f"/dev/mtd{mtd}", 0, len(data))
            if got != _md5(data):
                return Unsatisfied(f"mtd{mtd} != bundle {role}")
            ev[key] = got
        return Satisfied(ev)

    def run(self, ctx):
        t = ctx.need_linux()
        store = ctx.open_payload_store(t)
        for mtd, role, limit in ((0, "bl2", None), (1, "fip", gates.CM33_REGION_OFFSET)):
            p = ctx.artefact(role)
            ctx.mutate(f"mtd{mtd} <- {role} {p.name} ({p.stat().st_size} bytes): "
                       "flash_erase, mtd_debug write, md5 readback",
                       lambda m=mtd, q=p, lim=limit: lt.mtd_write_verify(t, m, q, lim, store))
        return self.result(ctx, "bl2 -> mtd0, fip -> mtd1 (CM33 region untouched)",
                           payload_store.evidence(store) if ctx.execute else None)


class WriteCm33(Step):
    """The CM33 image into mtd1 at 0x1A0000, where BL2 loads it from on an xSPI boot. The
    bundle stores the padded image, so the md5 readback covers exactly what is written."""
    name = "write_cm33"

    @staticmethod
    def _has_image(ctx) -> bool:
        return any(c.get("role") == "cm33" for c in ctx.bundle.get("components", []))

    def probe(self, ctx):
        if not self._has_image(ctx):
            return Satisfied(reason="bundle has no cm33 component")
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        data = ctx.artefact_bytes("cm33")
        got = t.md5("/dev/mtd1", gates.CM33_REGION_OFFSET, len(data))
        if got != _md5(data):
            return Unsatisfied(f"mtd1+{gates.CM33_REGION_OFFSET:#x} != bundle cm33")
        return Satisfied({"xspi_cm33_md5": got, "xspi_cm33_size": str(len(data))})

    def run(self, ctx):
        t = ctx.need_linux()
        store = ctx.open_payload_store(t)
        p = ctx.artefact("cm33")
        ctx.mutate(f"mtd1+{gates.CM33_REGION_OFFSET:#x} <- cm33 {p.name} ({p.stat().st_size} bytes): "
                   "flash_erase, mtd_debug write, md5 readback",
                   lambda: lt.mtd_write_verify(t, 1, p, store=store, offset=gates.CM33_REGION_OFFSET))
        return self.result(ctx, f"cm33 -> mtd1+{gates.CM33_REGION_OFFSET:#x} (BL2 starts it on the next xSPI boot)",
                           payload_store.evidence(store) if ctx.execute else None)


class WriteEmmcBoot(Step):
    name = "write_emmc_boot"

    def probe(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        emmc = lt.resolve_emmc(t)
        ev = {}
        for key, role, sector in (("emmc_boot1_bl2_md5", "bl2_mmc", gates.BL2_MMC_SECTOR),
                                  ("emmc_boot1_fip_md5", "fip", gates.FIP_SECTOR)):
            data = ctx.artefact_bytes(role)
            got = t.md5(f"{emmc}{lt.EMMC_BOOT_PART}", sector * 512, len(data))
            if got != _md5(data):
                return Unsatisfied(f"{emmc}{lt.EMMC_BOOT_PART} sector {sector:#x} != bundle {role}")
            ev[key] = got
        regs = lt.ext_csd(t, emmc)
        if regs[177] != 0x02 or regs[179] != 0x08:
            return Unsatisfied(f"EXT_CSD [177]={regs[177]:#04x} [179]={regs[179]:#04x}")
        ev.update(emmc_ext_csd_177="0x02", emmc_ext_csd_179="0x08")
        return Satisfied(ev)

    def run(self, ctx):
        t = ctx.need_linux()
        emmc = lt.resolve_emmc(t) if t else "/dev/<emmc>"
        store = ctx.open_payload_store(t)
        for role, sector in (("bl2_mmc", gates.BL2_MMC_SECTOR), ("fip", gates.FIP_SECTOR)):
            p = ctx.artefact(role)
            ctx.mutate(f"{emmc}{lt.EMMC_BOOT_PART} sector {sector:#x} <- {role} {p.name} (force_ro cleared for the write)",
                       lambda q=p, s=sector: lt.emmc_boot_write_verify(t, emmc, q, s, store))
        ctx.mutate(f"mmc-utils on {emmc}: EXT_CSD[177]=0x02, [179]=0x08",
                   lambda: lt.set_boot_config(t, emmc))
        return self.result(ctx, "release bl2_mmc + fip in eMMC boot partition 1 (Linux boot0); boot config set",
                           payload_store.evidence(store) if ctx.execute else None)


def _wic_md5(ctx: Ctx) -> tuple[str, int]:
    if "wic_md5" not in ctx._cache:
        import gzip
        h, n = hashlib.md5(), 0
        with gzip.open(ctx.artefact("system_image"), "rb") as f:
            for blk in iter(lambda: f.read(1 << 20), b""):
                h.update(blk)
                n += len(blk)
        ctx._cache["wic_md5"] = (h.hexdigest(), n)
    return ctx._cache["wic_md5"]


def _bmap(ctx: Ctx):
    """The parsed system_image_bmap, or None when the bundle carries none."""
    if not any(c.get("role") == "system_image_bmap" for c in ctx.bundle.get("components", [])):
        return None
    if "bmap" not in ctx._cache:
        ctx._cache["bmap"] = bmap.parse(ctx.artefact("system_image_bmap"))
    return ctx._cache["bmap"]


class WriteRootfs(Step):
    name = "write_rootfs"

    def probe(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        emmc = lt.resolve_emmc(t)
        if (bm := _bmap(ctx)) is not None:
            if "bmap_md5" not in ctx._cache:
                ctx._cache["bmap_md5"] = bmap.stage(ctx.artefact("system_image"), bm, None)[0]
            lt.put_ranges(t, bm)
            try:
                got = lt.md5_ranges(t, emmc, bm)
            finally:
                t.run(f"rm -f {lt.RANGES_PATH}", check=False)
            if got != ctx._cache["bmap_md5"]:
                return Unsatisfied(f"{emmc} does not hold the bundle wic's mapped ranges")
            return Satisfied({})
        want, n = _wic_md5(ctx)
        if t.md5(emmc, 0, n) != want:
            return Unsatisfied(f"{emmc} does not hold the bundle wic")
        return Satisfied({})

    def run(self, ctx):
        t = ctx.need_linux()
        wic = ctx.artefact("system_image")
        if t is not None:
            emmc = lt.resolve_emmc(t)
            root = lt.root_device(t)
            if root.startswith(emmc):
                raise Refused(f"Linux root {root} is on the eMMC {emmc}; refusing to overwrite the running root")
        else:
            emmc = "/dev/<emmc>"
        dtb = gates.fip_fdtfile(ctx.artefact_bytes("fip"))
        bm = _bmap(ctx)
        ev: dict[str, str] = {}
        store = ctx.open_payload_store(t)
        why = ("the bundle has no system_image_bmap" if bm is None
               else "the board has no python3" if t is not None and not lt.have_python3(t) else "")
        if why:
            ctx.plan_log.append(f"{why}: full-image write instead of bmap")
            ctx.mutate(f"gunzip -c {wic.name} | dd of={emmc} bs=4M && sync (over SSH), md5 readback",
                       lambda: ev.update(lt.rootfs_write_verify(t, emmc, wic, store=store),
                                         rootfs_write_mode="full-image", rootfs_bmap_fallback=why))
        else:
            ctx.mutate(f"bmap: write only the mapped ranges ({bm.mapped_bytes} of {bm.image_size} bytes, "
                       f"{len(bm.ranges)} ranges) of {wic.name} on the board via python3 (ranges "
                       "host-verified first), md5 readback of the same ranges",
                       lambda: ev.update(lt.rootfs_write_verify_mapped(t, emmc, wic, bm, store=store),
                                         rootfs_write_mode="bmap-python"))

        def check():
            name = emmc.rsplit("/", 1)[-1]
            parts = t.run(f"ls -d /sys/block/{name}/{name}p*").stdout.split()
            errs = []
            for p in sorted(int(x.rsplit("p", 1)[1]) for x in parts):
                try:
                    lt.rootfs_check(t, emmc, p, dtb)
                    return p
                except BenchError as e:
                    errs.append(f"p{p}: {e}")
            raise BenchError(f"no partition passes fsck + /boot/{dtb}: {'; '.join(errs)}")
        part = ctx.mutate(f"fsck -n, mount ro,noload, /boot/{dtb} present", check)
        how = (f"{ev['rootfs_bytes_written']} of {ev['rootfs_image_bytes']} bytes, {ev['rootfs_write_mode']}"
               if ev else "plan")
        if ctx.execute:
            ev.update(payload_store.evidence(store))
        return self.result(ctx, f"wic written ({how}); rootfs p{part} holds /boot/{dtb}" if part
                           else "would write the wic and check the rootfs", ev)


def _latest_entry(ctx, name: str) -> dict | None:
    """The newest state-file entry of step ``name``: this run's group, else the newest
    superseded group that holds one. Whatever its status."""
    cur = ctx.state.get("steps", {}).get(name)
    if cur is not None:
        return cur
    g = next((g for g in reversed(ctx.state.get("superseded", [])) if name in g.get("steps", {})), None)
    return g["steps"][name] if g else None


class Census(Step):
    name = "census"
    always_run = True
    # CensusFinal: the unit runs its shipping image after a plain cold boot, where
    # cold_boot_test owns the ACT88760 GPIO4 keys (reg 0x10 reads released whoever released it).
    reads_gpio4_otp = True

    def run(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return self.result(ctx, "no Linux target: census deferred", status="skipped")
        bus = {k: ctx.bench.i2c_bus.get(k) if ctx.bench else None for k in ("eeprom", "pmic", "brd")}
        bus["eeprom"] = bus["eeprom"] if bus["eeprom"] is not None else 0
        sizes = {r: len(ctx.artefact_bytes(r)) for r in ("bl2", "fip", "bl2_mmc")
                 if any(c.get("role") == r for c in ctx.bundle.get("components", []))}
        if why := leftover_dxuart2_swap(ctx):
            raise Refused(why)
        facts, notes = lt.census(t, bus, sizes, dxm1_present=ctx.family == "v2n-m1")
        if bus["pmic"] is not None and self.reads_gpio4_otp:
            try:
                # gd32_flash runs BEFORE census. When it applied the volatile
                # release it already recorded the OTP value it saw (0x88) and
                # the workaround; reading reg 0x10 now would see the released
                # 0x08 and overwrite both, so census leaves them alone then.
                # Otherwise no release happened and 0x08 is conclusive; "u-boot"
                # is decided later, from cold_boot_test's post-cold-cycle read.
                if not str(ctx.facts.get("act88760_gpio4_workaround", "")).startswith("provision"):
                    raw = lt.i2c_get(t, bus["pmic"], lt.ACT88760_ADDR, lt.ACT88760_GPIO_REG)
                    facts["act88760_gpio4_otp"] = f"{raw:#04x}"
                    if raw == lt.ACT88760_GPIO4_RELEASED:
                        facts["act88760_gpio4_workaround"] = "none"
            except BenchError as e:
                facts["act88760_gpio4_otp"] = lt.unread(e)
                notes.append(f"act88760_gpio4_otp: {e}")
        warn = ""
        if ctx.bench is not None:
            try:
                amps = ctx.bench.power.current()
            except BenchError as e:
                facts["psu_current_a"] = lt.unread(e)
                notes.append(f"psu_current_a: {e}")
            else:
                if amps is not None and not math.isfinite(amps):
                    facts["psu_current_a"] = lt.unread(f"not a finite number: {amps}")
                    notes.append(f"psu_current_a: not a finite number: {amps}")
                elif amps is not None:
                    facts["psu_current_a"] = f"{amps:.3f}"
                    facts["psu_current_state"] = "at-census"     # one reading taken here; load not controlled
                    if amps > PSU_CURRENT_WARN_A:
                        # a screen, not a gate: the real threshold belongs in the private catalogue
                        warn = (f"WARNING: supply current {amps:.3f} A > {PSU_CURRENT_WARN_A:.2f} A at "
                                "census (normal 0.17-0.29 A at idle): suspect the PHY regulator output "
                                "capacitor fault")
        if any(str(v).startswith("unread") for v in facts.values()):
            # name the usual cause of unread I2C keys instead of leaving a bare read error
            try:
                n = t.run(f"dmesg | grep -c '{I2C_WEDGE_DMESG}'", check=False).stdout.strip()
            except BenchError:
                n = ""
            if n.isdigit() and int(n):
                notes.append(f"I2C bus wedged: {n} '{I2C_WEDGE_DMESG}' line(s) in dmesg (a GD32 left halted "
                             "over SWD holds SCL); the bus stays dead until the next power cycle, "
                             "census_final re-reads these keys after the final cold boot")
        ctx.step_logs[self.name] = "\n".join(notes + ([warn] if warn else []))
        return self.result(ctx, f"{len(facts)} keys" + (f"; unread: {'; '.join(notes)}" if notes else "")
                           + (f"; {warn}" if warn else ""), facts, status="done")


class CensusFinal(Census):
    """The census again, on the unit as shipped: after cold_boot_test's last cold boot (xSPI
    boot, eMMC root). Its values replace the earlier census's, so a key the first census could
    not read (`unread (...)`, e.g. on a wedged I2C bus) is read here; one that is still unread
    stays unread and blocks the ship check."""
    name = "census_final"
    reads_gpio4_otp = False
    NOT_REREAD = "not re-read by census_final"

    def run(self, ctx):
        res = super().run(ctx)
        if res.status != "done":
            return res
        # A census group that fails drops its keys, and a key that is absent here would leave
        # the first census's value (read in the microSD boot) standing in the ledger. Every key
        # the first census recorded and this one did not produce is therefore marked unread.
        first = (_latest_entry(ctx, Census.name) or {}).get("evidence") or {}
        lost = sorted(k for k in first if k not in res.evidence and not k.startswith("act88760_gpio4_")
                      and k != "power_cut")
        for k in lost:
            res.evidence[k] = lt.unread(self.NOT_REREAD)
        if lost:
            res.detail += f"; {len(lost)} key(s) of the first census not re-read, marked unread: {', '.join(lost)}"
            ctx.step_logs[self.name] = "\n".join(filter(None, [ctx.step_logs.get(self.name, ""),
                                                               "not re-read: " + ", ".join(lost)]))
        return res


class EepromManifest(Step):
    name = "eeprom_manifest"

    def probe(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        arr = lt.eeprom_read(t, ctx.i2c("eeprom"), 0, lt.MANIFEST_LEN)
        written = ctx.unit_dir / f"{ctx.serial}.manifest.bin"
        if written.is_file() and written.read_bytes() == arr:
            return Satisfied(_manifest_ev(arr))
        if arr == b"\xff" * lt.MANIFEST_LEN:
            return Unsatisfied("array blank")
        return Unsatisfied("array holds a manifest that is not this unit's committed manifest.bin")

    def preconditions(self, ctx, t) -> list[str]:
        """Read-only. Every failed precondition, named."""
        bus = ctx.i2c("eeprom")
        bad = []
        lock = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK_STATUS_READ))[0]
        if lock & LOCK_BIT:
            bad.append(f"identity header locked (lock status {lock:#04x})")
        arr = lt.eeprom_read(t, bus, 0, lt.MANIFEST_LEN)
        if arr != b"\xff" * lt.MANIFEST_LEN:
            if ctx.reprovision_from is None:
                bad.append("array 0x0000..0x007f is not blank (pass --reprovision-from the manifest it holds)")
            elif Path(ctx.reprovision_from).read_bytes() != arr:
                bad.append(f"array does not equal --reprovision-from {Path(ctx.reprovision_from).name}")
            sku = gates.sku_triangle(ctx.sku, ctx.bundle.get("sku", ""), ctx.preset.get("sku", ""),
                                     _manifest_sku(arr))
            if not sku.ok:
                bad.append(sku.detail)
        if ctx.family == "v2n-m1":
            r = t.run(f"dd if=/dev/mtd1 bs=4096 count={gates.CM33_REGION_OFFSET // 4096} 2>/dev/null | grep -q -a -F "
                      f"{shlex.quote(gates.RAIL_PG)}", check=False)
            if r.rc != 0:
                bad.append(f"FIP on xSPI mtd1 lacks {gates.RAIL_PG!r} (U-Boot patch 0004): a v2n-m1 "
                           "manifest would release M1_RESET with the DEEPX rail unmanaged")
        return bad

    def run(self, ctx):
        t = ctx.need_linux()
        if t is not None:
            bad = self.preconditions(ctx, t)
            if bad:
                raise Refused("refused: " + "; ".join(bad))
        else:
            ctx.plan_log.append("UNCHECKED: lock bit, blank array, SKU leg, xSPI rail string (no Linux target)")
        blob = _build_blob(ctx, "program_eeprom.py", "manifest.bin")
        ev = {"manifest_staged_crc32": _crc(blob[:0x7C]),
              "manifest_staged_sha256": hashlib.sha256(blob).hexdigest()}
        bus = ctx.i2c("eeprom")
        ctx.mutate(f"stage {ctx.serial}.manifest.staged.bin in the ledger",
                   lambda: _stage(ctx, f"{ctx.serial}.manifest.staged.bin", blob))
        ctx.mutate(f"write 128-byte manifest: 8 x 16-byte pages, i2c-{bus} @0x50, ACK poll, readback",
                   lambda: lt.eeprom_write_pages(t, bus, 0, blob))

        def recheck():
            # the MAC just changed (CID- to serial-derived): DHCP may hand a new IP
            ctx.rediscover_host = True
            boot_to_linux(ctx, rediscover=True)
            got = lt.eeprom_read(ctx.linux, bus, 0, lt.MANIFEST_LEN)
            if got != blob:
                raise BenchError("manifest differs after the cold cycle")
        ctx.mutate("cold cycle, log in, re-read and re-compare the 128 bytes", recheck)
        ctx.mutate(f"promote {ctx.serial}.manifest.staged.bin -> {ctx.serial}.manifest.bin",
                   lambda: ledger_out.promote_manifest(ctx.unit_dir, ctx.serial))
        if ctx.execute:
            ev.update(_manifest_ev(blob))
        return self.result(ctx, f"manifest for {ctx.serial}, mfg_date {ctx.mfg_date}", ev)


def _manifest_ev(arr: bytes) -> dict[str, str]:
    return {"manifest_crc32": f"0x{int.from_bytes(arr[0x7C:0x80], 'little'):08x}",
            "manifest_sha256": hashlib.sha256(arr).hexdigest()}


class Gd32Flash(Step):
    name = "gd32_flash"

    def _images(self, ctx):
        if ctx.gd32_fw is None:
            raise Refused("no --gd32-fw DIR (bootloader.bin, ota-meta.bin, slot-a.bin)")
        return [(Path(ctx.gd32_fw) / f, a, k) for f, a, k in GD32_IMAGES]

    @staticmethod
    def _resume(ctx, probe) -> str:
        """Reset-and-run after anything that halts the core: a savebin dump always does, a
        loadbin does when it fails part-way. A halted GD32 still ACKs 0x70 and stretches SCL,
        which wedges the board-management I2C bus for the rest of the boot. Never raises (it
        runs in ``finally``); returns "" or the error."""
        ctx.plan_log.append("reset/run the GD32 (a readback or a failed write leaves the core halted)")
        try:
            probe.reset_run()
            ctx._cache.pop("gd32_halted", None)
            return ""
        except Exception as e:      # noqa: BLE001 -- runs on a failure path: must never replace that failure
            err = f"{type(e).__name__}: {e}" if not isinstance(e, BenchError) else (str(e) or "reset-run failed")
            ctx.plan_log.append(f"NOTE: GD32 reset-and-run FAILED, the core may still be halted: {err}")
            ctx._cache["gd32_halted"] = err      # run() refuses to go on with a core it knows is halted
            return err

    @staticmethod
    def _bridge_alive(ctx, t) -> str:
        """GET_VERSION from the bridge, polled BRIDGE_TRIES times BRIDGE_GAP_S apart.
        Returns "GD32 bridge protocol a.b.c"; BenchError when it never answers."""
        bus, err = ctx.i2c("brd"), None
        for attempt in range(BRIDGE_TRIES):
            try:
                return "GD32 bridge protocol %d.%d.%d" % lt.gd32_bridge_version(t, bus, GD32_BRIDGE_ADDR)
            except BenchError as e:
                err = e
            if attempt < BRIDGE_TRIES - 1:
                time.sleep(BRIDGE_GAP_S)
        raise BenchError(f"the GD32 bridge at {GD32_BRIDGE_ADDR:#04x} did not answer GET_VERSION in "
                         f"{BRIDGE_TRIES} tries after the reset-and-run: i2c-{bus} may be wedged by a core "
                         f"left halted over SWD (dmesg: '{I2C_WEDGE_DMESG}'); power-cycle the unit. Last error: {err}")

    def _readback(self, ctx, images, probe=None, resume: bool = True) -> dict[str, str]:
        """md5 of each image region, read with savebin. savebin HALTS the core and leaves it
        halted, so the core is reset-and-run afterwards, also when a savebin raises
        (``resume=False`` only for a caller that does the same in its own ``finally``)."""
        probe = probe or ctx.bench.probe
        ev = {}
        try:
            with tempfile.TemporaryDirectory(prefix="gd32_") as td:
                for i, (p, addr, key) in enumerate(images):
                    out = Path(td) / f"rb{i}.bin"
                    probe.savebin(out, addr, p.stat().st_size)   # fresh session each
                    ev[key] = _md5(out.read_bytes())
        except (BenchError, OSError) as e:
            # the readback's own error stays the reason; a failed resume is appended to it
            if resume and (err := self._resume(ctx, probe)):
                raise BenchError(f"{e}; ALSO the reset-and-run failed, GD32 core left halted: {err}") from e
            raise
        if resume and (err := self._resume(ctx, probe)):
            raise BenchError(f"GD32 core left halted: the reset-and-run after the readback failed: {err}")
        return ev

    def probe(self, ctx):
        if ctx.bench is None or ctx.bench.probe is None or ctx.gd32_fw is None:
            return Unknown("no probe or no --gd32-fw")
        if (ctx.execute and ctx.linux is None and not ctx.bench.linux_host
                and getattr(ctx.bench.probe, "env", None) is not None):
            # --only gd32_flash starts past the step that attaches the Linux target;
            # the probe wrapper needs ALP_PROVISION_HOST, so discover it over the console.
            try:
                console_login_ctx(ctx, timeout=20.0)
                connect_linux(ctx)
            except BenchError:
                pass   # no shell / no IP: the probe runs without a host, as before
        try:
            dp = ctx.bench.probe.dp_id()
            if dp != GD32_DP_OK:
                return Unknown(f"DP-ID {dp:#010x} is not the GD32 ({GD32_DP_OK:#010x}); no readback")
            images = self._images(ctx)
            ev = self._readback(ctx, images)
        except (BenchError, OSError) as e:
            return Unknown(f"probe readback: {e}")
        for p, _a, key in images:
            if ev[key] != _md5(p.read_bytes()):
                return Unsatisfied(f"{p.name} not on the GD32")
        # The images are on the chip and the core was just reset: it must answer again, or the
        # readback left the bus wedged and the next step (census) would read nothing on it.
        # Without a Linux target (an external probe, unit not booted) nobody can ask.
        if ctx.linux is not None:
            try:
                ev["gd32_bridge_after_readback"] = self._bridge_alive(ctx, ctx.linux)
            except (BenchError, Refused) as e:
                return Unknown(f"firmware matches, but {e}")
        return Satisfied({**ev, "gd32_dp_id": f"0x{dp:08x}", **self._fw_version(ctx)})

    @staticmethod
    def _fw_version(ctx) -> dict[str, str]:
        """`<gd32-fw>/VERSION` names the release the images came from; the md5
        readback above is what ties that name to the bytes on the chip."""
        v = Path(ctx.gd32_fw) / "VERSION"
        return {"gd32_fw_version": v.read_text(encoding="utf-8").strip()} if v.is_file() else {}

    @staticmethod
    def _transport(ctx):
        """(target, probe, via_console). SSH and the bench's own probe wrapper
        are preferred. With no network and
        a root shell on the console, the SWD tools are pushed over the console."""
        b = ctx.need_bench()
        if not ctx.execute:
            # plan only: no probe or console traffic, but name the transport execute would take
            return None, None, not ctx.linux_up()
        if ctx.linux_up():
            return ctx.need_linux(), b.probe, False
        if b.console_swd is None:
            raise Refused("no network and bench.yaml has no script probe to push over the console")
        tools_dir, tools = b.console_swd
        missing = [f for f in tools if not (tools_dir / f).is_file()]
        if missing:
            raise Refused(f"no network and the console-push SWD tools are missing in {tools_dir}: {missing}")
        console_login_ctx(ctx)
        t = ConsoleTarget(b.console)
        ctx._check_unit_identity(t)          # the same eMMC-CID gate the SSH path gets in need_linux
        store = ctx.open_payload_store(t)
        ctx._cache["gd32_store"] = store
        return t, StoreSwdProbe(t, tools_dir, tools, store or payload_store.PayloadStore(t, None)), True

    def _plan(self, ctx, ev, via_console):
        """Dry run: WOULD lines only. The probe wrapper is never invoked (it needs
        a host/IP the dry run does not have)."""
        images = self._images(ctx)
        if via_console:
            ctx.mutate("no reachable IP: console login, open the SD payload store and copy the SWD tools "
                       "and images from it (sha256-checked on the board); any file missing or "
                       "mismatching is pushed over the console instead (base64, md5-checked) and "
                       "cached in the store", lambda: None)
        ctx.mutate(f"DP-ID gate ({GD32_DP_OK:#010x} only)", lambda: None)
        for p, addr, _key in images:
            ctx.mutate(f"loadbin {p.name} @ {addr:#010x}", lambda: None)
        ctx.mutate("verify each region with savebin in a FRESH probe session, md5", lambda: None)
        ctx.mutate("reset/run the GD32 (also after a failed write or verify)", lambda: None)
        ctx.mutate(f"GET_VERSION from the bridge at {GD32_BRIDGE_ADDR:#04x} (polled)", lambda: None)
        if via_console:
            ctx.mutate("cold cycle; re-check the IP", lambda: None)
        return self.result(ctx, "would flash the GD32", ev)

    def run(self, ctx):
        ev: dict[str, str] = {}
        t, probe, via_console = self._transport(ctx)
        if via_console:
            ev["gd32_flash_transport"] = "console (no network)"
        if not ctx.execute:
            return self._plan(ctx, ev, via_console)
        if halted := ctx._cache.get("gd32_halted"):
            # the pre-run probe's readback halted the core and its reset failed: say so in the
            # step result instead of running into a dead I2C bus
            raise BenchError(f"GD32 core left halted by the pre-run probe's readback (the reset-and-run failed: "
                             f"{halted}); i2c-{ctx.i2c('brd')} is wedged until the next power cycle: "
                             "power-cycle the unit and re-run")
        if t is not None:
            pmic = ctx.i2c("pmic")
            try:
                held = lt.act88760_gpio4_held(t, pmic)
            except BenchError as e:
                # the pre-run probe's readback may have left the bus dead: say so, write nothing
                raise BenchError(f"{e}; i2c-{pmic} is unreadable before the flash: if dmesg shows "
                                 f"'{I2C_WEDGE_DMESG}', a GD32 left halted over SWD holds the bus; "
                                 "power-cycle the unit and re-run") from e
            if held:
                ev["act88760_gpio4_otp"] = f"{lt.ACT88760_GPIO4_OTP_DEFAULT:#04x}"
                ev["act88760_gpio4_workaround"] = "provision (volatile 0x08)"
                ctx.mutate("ACT88760 0x25 reg 0x10 = 0x08 (volatile GPIO4 / GD32_NRST release)",
                           lambda: lt.act88760_gpio4_release(t, pmic))
        b = ctx.need_bench()
        if probe is None:
            raise Refused("bench.yaml has no probe: this bench has no SWD path to the GD32")
        images = self._images(ctx)
        dp = probe.dp_id()
        ev["gd32_dp_id"] = f"0x{dp:08x}"
        if dp != GD32_DP_OK:
            why = GD32_DP_REFUSE.get(dp, "an unknown debug port")
            raise Refused(f"DP-ID {dp:#010x} is {why}; want {GD32_DP_OK:#010x}")
        def verify():
            got = self._readback(ctx, images, probe, resume=False)     # resumed in the finally below
            for p, _a, key in images:
                if got[key] != _md5(p.read_bytes()):
                    raise BenchError(f"GD32 readback of {p.name} does not match")
            return got
        try:
            for p, addr, _key in images:
                ctx.mutate(f"loadbin {p.name} @ {addr:#010x}", lambda q=p, a=addr: probe.loadbin(q, a))
            ev.update(ctx.mutate("verify each region with savebin in a FRESH probe session, md5", verify) or {})
        except (BenchError, Refused, ValueError, OSError) as e:
            # a failed loadbin or verify must not leave the core halted either; its own error
            # stays the reason and a failed resume is appended to it
            if err := self._resume(ctx, probe):
                cls = Refused if isinstance(e, Refused) else BenchError
                raise cls(f"{e}; ALSO the reset-and-run failed, GD32 core left halted: {err}") from e
            raise
        if resume_err := self._resume(ctx, probe):
            raise BenchError(f"GD32 written and verified, but the reset-and-run failed (core left halted): {resume_err}")
        # The bridge must answer before the step returns: census reads the same bus next.
        if t is not None:
            ev["gd32_protocol"] = ctx.mutate(f"GET_VERSION from the bridge at {GD32_BRIDGE_ADDR:#04x} (polled)",
                                             lambda: self._bridge_alive(ctx, t))
        ev.update(self._fw_version(ctx))
        # the SSH path's probe wrapper scp's from the host; only the console path reads the store
        ev.update(payload_store.evidence(ctx._cache.get("gd32_store") if via_console else None))
        if via_console:
            # The flash evidence is complete above; a failed re-check must not lose it.
            try:
                ctx.mutate("cold cycle; the GD32 now runs, so gbeth has its RX clock; re-check the IP",
                           lambda: cold_boot_phy_retry(ctx, ev))
                if ctx.execute:
                    ev["network_after_gd32"] = ctx.linux.host
            except BenchError as e:
                ev["network_after_gd32"] = f"none: {e}"
                return self.result(ctx, f"GD32 flashed and verified, but no network after the cold cycle: {e}",
                                   ev, status="failed")
        return self.result(ctx, "GD32 flashed and verified" if ctx.execute else "would flash the GD32", ev)


def warm_reboot_to_linux(ctx: Ctx) -> None:
    """Warm `reboot` from Linux (no PSU action), wait for the console login, re-attach.
    The DX-M1 DTB swap needs only a new device tree, not a power cycle."""
    b = ctx.need_bench()
    n = len(b.console.transcript)
    b.console.drain()
    ctx.linux.run("sync; (sleep 1; reboot) </dev/null >/dev/null 2>&1 &", check=False)
    boot_to_linux(ctx, resume_mark=n)


def poweroff_and_cold_boot(ctx: Ctx) -> str:
    """Clean `poweroff` and the usual cold cycle and login, all in boot_to_linux
    (clean_shutdown, then Power.cycle with MIN_ON_S / MIN_OFF_S)."""
    ctx.need_bench()
    return boot_to_linux(ctx)


def _norm_ver(v: str) -> str:
    return v.strip().lstrip("vV")


class Dxm1NpuFlash(Step):
    """DX-M1 NPU firmware into its SPI-NAND over the ROM's UART path (V2M / v2n-m1).

    Needs the BOOT_CFG straps in mode 0 (E1M IO17/IO19/IO20 low; the X-EVK needs a
    carrier rework) and the license-gated firmware files from the release bundle
    (``dxm1_*`` roles; without them the step is skipped). Run order: push files,
    swap the release DTB for the dxuart2 DTB (UART on, PCIe off) and warm reboot,
    [sf_erase when boot2nd already runs], dxflash.py + PA6 reset pulse, restore the
    release DTB (md5-verified, also on failure), clean poweroff + cold cycle, verify
    PCIe device 0x0000 and the ``dxrt-cli -s`` firmware version. See dxm1.py."""
    name = "dxm1_npu_flash"

    @staticmethod
    def _component(ctx, role):
        return next((c for c in ctx.bundle.get("components", []) if c.get("role") == role), None)

    def _inapplicable(self, ctx) -> str:
        if ctx.family != "v2n-m1":
            return "not a V2M/DEEPX SKU: no DX-M1 to flash"
        missing = [r for r in dxm1.REQUIRED_ROLES if self._component(ctx, r) is None]
        if missing:
            return (f"bundle {ctx.bundle.get('release_version', '?')} carries no DX-M1 firmware "
                    f"(missing {', '.join(missing)})")
        return ""

    def _expected_version(self, ctx) -> str:
        v = self._component(ctx, dxm1.ROLE_FW).get("version")
        if not v:
            raise Refused(f"bundle component {dxm1.ROLE_FW} declares no version: nothing to verify the flash against")
        return _norm_ver(v)

    def _evidence(self, ctx, version: str) -> dict[str, str]:
        return {"dxm1_fw_version": version, "dxm1_fw_md5": _md5(ctx.artefact_bytes(dxm1.ROLE_FW)),
                "dxm1_fw_uart_boot_md5": _md5(ctx.artefact_bytes(dxm1.ROLE_UART_BOOT))}

    @staticmethod
    def _recorded_md5(ctx) -> str:
        """The dxm1_fw_md5 last recorded for THIS unit (the NAND content is the unit's, not
        the bundle's): this run's / the newest earlier state entry that finished, else the
        ledger unit.yaml. "" when nothing was ever recorded."""
        name = Dxm1NpuFlash.name
        if "dxm1_flashed_md5" in ctx._cache:        # flashed and verified by this very run
            return ctx._cache["dxm1_flashed_md5"]
        groups = [ctx.state.get("steps", {})] + [g.get("steps", {}) for g in reversed(ctx.state.get("superseded", []))]
        for steps_ in groups:
            if name not in steps_:
                continue
            st = steps_[name]
            if st.get("status") not in ("done", "skipped"):
                return ""           # the newest entry failed / was interrupted: the NAND is unknown (as Record)
            if (st.get("evidence") or {}).get("dxm1_fw_md5"):
                return st["evidence"]["dxm1_fw_md5"]
        return ledger_out.read_unit_yaml(ctx.unit_dir / f"{ctx.serial}.unit.yaml").get("dxm1_fw_md5", "").strip()

    def probe(self, ctx):
        if why := self._inapplicable(ctx):
            return Satisfied({}, why)
        version = self._expected_version(ctx)
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        dev = lt.dxm1_pcie_device(t)
        if dev != lt.DXM1_PCIE_FW_RUNNING:
            return Unsatisfied(f"DX-M1 PCIe device {dev or 'absent'}, want {lt.DXM1_PCIE_FW_RUNNING} (firmware running)")
        got = lt.dxm1_fw_version(t)
        if got is None or _norm_ver(got) != version:
            return Unsatisfied(f"DX-M1 firmware {got or 'unreadable'} != bundle {version}")
        # two firmware variants can report the same version: only the md5 tells them apart
        want, rec = _md5(ctx.artefact_bytes(dxm1.ROLE_FW)), self._recorded_md5(ctx)
        if not rec:
            return Unsatisfied("no dxm1_fw_md5 recorded for this unit: cannot tell which firmware variant "
                               f"the NAND holds (bundle {want})")
        if rec != want:
            return Unsatisfied(f"the NAND holds firmware md5 {rec}, bundle has {want}")
        return Satisfied({**self._evidence(ctx, version), "dxm1_pcie_device": dev})

    @staticmethod
    def _heal(ctx, dtb_name: str, st: dict) -> str:
        """After a failure past the DTB swap: put the release DTB back (best effort)."""
        if "release" not in st or not ctx.execute:
            return ""
        note = ""
        if "gpio" in st:
            chip, mux, rst = st["gpio"]
            try:
                if dxm1.kill_dxflash(ctx.linux):     # never tear the GPIOs down under a live flasher
                    note = f"; {dxm1.unexport_gpios(ctx.linux, chip, (mux, rst))}"
                else:
                    note = "; flasher still alive after pkill -9, GPIOs left exported"
            except (BenchError, AttributeError) as e:
                note = f"; GPIOs left exported ({e})"
        try:
            dxm1.restore_dtb(ctx.linux, dtb_name, st["release"])
            return note + "; release DTB restored (the unit runs the dxuart2 DTB until its next cold cycle)"
        except (BenchError, AttributeError) as e:
            return note + f"; ALSO could not restore the release DTB: {e}"

    def run(self, ctx):
        if why := self._inapplicable(ctx):
            return self.result(ctx, why, status="skipped")
        version = self._expected_version(ctx)
        try:
            chip, mux, rst = dxm1.gpio_config(ctx.need_bench().raw)
        except ValueError as e:
            raise Refused(str(e)) from e
        t = ctx.need_linux()
        dtb_name = gates.fip_fdtfile(ctx.artefact_bytes("fip"))
        before = lt.dxm1_pcie_device(t) if t is not None else None
        # boot2nd already running from the NAND (device 0x0000): erase before reprogramming
        erase = before == lt.DXM1_PCIE_FW_RUNNING
        if erase and self._component(ctx, dxm1.ROLE_DXCLI) is None:
            raise Refused("the DX-M1 NAND already holds boot2nd (PCIe device 0x0000) but the bundle has no "
                          f"{dxm1.ROLE_DXCLI} (dxcli.py) to run sf_erase 0 1000000 first")
        # /tmp is tmpfs: the warm reboot wipes it. Only the DTB (copied into /boot before
        # the reboot) is pushed first; every flash payload is pushed after the reboot.
        payload = [r for r in dxm1.REQUIRED_ROLES if r != dxm1.ROLE_DTB] + ([dxm1.ROLE_DXCLI] if erase else [])
        ev = self._evidence(ctx, version)
        st: dict = {}

        stores: list = []     # one per boot

        def push_dtb():
            stores.append(ctx.open_payload_store(t))
            dxm1.push(t, ctx.artefact(dxm1.ROLE_DTB), dxm1.REMOTE[dxm1.ROLE_DTB], stores[-1])

        def push_payload():
            stores.append(ctx.open_payload_store(ctx.linux))
            for r in payload:
                dxm1.push(ctx.linux, ctx.artefact(r), dxm1.REMOTE[r], stores[-1])

        def install():
            dxm1.install_dtb(t, dtb_name, ctx.artefact(dxm1.ROLE_DTB), st)

        def flash():
            tt = ctx.linux
            st["gpio"] = (chip, mux, rst)
            if erase:
                ev["dxm1_nand_erase"] = dxm1.erase_nand(tt, chip, mux, rst)
            rc, log, timed_out = dxm1.run_dxflash(tt, chip, mux, rst)
            ev["dxm1_dxflash_rc"] = dxm1.rc_evidence(rc, log, timed_out)
            ctx.step_logs[self.name] = log
            return dxm1.classify(rc, log, timed_out)

        try:
            ctx.mutate("push the dxuart2 DTB to /tmp, md5-checked on the target", push_dtb)
            ctx.mutate(f"back up /boot/{dtb_name} as .release, install the dxuart2 DTB (UART on, PCIe off)", install)
            ctx.mutate("warm reboot onto the dxuart2 DTB", lambda: warm_reboot_to_linux(ctx))
            ctx.mutate("verify the dxuart2 DTB booted (serial@12801000 present, DX-M1 PCIe off)",
                       lambda: dxm1.check_dxuart2_booted(ctx.linux))
            ctx.mutate(f"push {', '.join(payload)} to /tmp AFTER the reboot (tmpfs), md5-checked on the target",
                       push_payload)
            ctx.mutate(f"{'sf_erase 0 1000000 via dxcli.py (NAND holds boot2nd); ' if erase else ''}"
                       f"P75 + PA6 high, dxflash.py in the background, PA6 low {dxm1.RESET_HOLD_S:g} s after "
                       f"{dxm1.RESET_AFTER_S:g} s, wait for 'update_firmware end. 0' + CRC, keep its real exit code",
                       flash)
        except dxm1.StrapError as e:
            raise Refused(f"{e}{self._heal(ctx, dtb_name, st)}") from e
        except Refused as e:
            raise Refused(f"{e}{self._heal(ctx, dtb_name, st)}") from e
        except (BenchError, OSError) as e:  # not type(e)(...): ExpectTimeout takes (pattern, tail, timeout)
            raise BenchError(f"{e}{self._heal(ctx, dtb_name, st)}") from e
        ctx.mutate(f"restore /boot/{dtb_name} from .release, verify its md5",
                   lambda: dxm1.restore_dtb(ctx.linux, dtb_name, st["release"]))
        ctx.mutate("clean poweroff, cold cycle, log in", lambda: poweroff_and_cold_boot(ctx))

        def verify():
            dev = lt.dxm1_pcie_device(ctx.linux)
            if dev != lt.DXM1_PCIE_FW_RUNNING:
                raise BenchError(f"DX-M1 PCIe device {dev or 'absent'} after the cold boot, want "
                                 f"{lt.DXM1_PCIE_FW_RUNNING} ({lt.DXM1_PCIE_ROM_BOOT} = the ROM's own PCIe boot: "
                                 "no firmware on the NAND)")
            got = lt.dxm1_fw_version(ctx.linux)
            if got is None or _norm_ver(got) != version:
                raise BenchError(f"dxrt-cli -s firmware {got or 'unreadable'} != bundle {version}")
            ev["dxm1_pcie_device"] = dev
            ctx._cache["dxm1_flashed_md5"] = ev["dxm1_fw_md5"]
        ctx.mutate("verify PCIe 0000:01:00.0 device 0x0000 and the dxrt-cli -s firmware version", verify)
        if ctx.execute:
            used = [x for x in stores if x is not None]
            ev["payload_source"] = ("sd-store" if used and all(x.summary() == "sd-store" for x in used)
                                    else "pushed")
            if notes := [n for x in used for n in x.notes]:
                ev["payload_store_note"] = "; ".join(notes)[:400]
        return self.result(ctx, f"DX-M1 firmware {version} programmed over the ROM UART path and verified"
                           if ctx.execute else f"would program the DX-M1 firmware {version} over the ROM UART path", ev)


class PmicVerify(Step):
    name = "pmic_verify"
    always_run = True

    def run(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return self.result(ctx, "no Linux target: register compare deferred", status="skipped")
        spec = ctx.expected_registers
        if not spec:
            raise Refused("no --pmic-expect (expected-registers) given")
        # census/gd32_flash may have observed reg 0x10 still at the OTP default
        # (workaround not applied/effective yet, e.g. a dry run) -- that is a
        # known, already-explained mismatch, not a new failure.
        otp_default_seen = ctx.facts.get("act88760_gpio4_otp") == f"{lt.ACT88760_GPIO4_OTP_DEFAULT:#04x}"
        bad, notes = [], []
        for dev, d in (spec.get("devices") or {}).items():
            fams = d.get("families")
            if fams and ctx.family not in fams:
                continue
            bus = ctx.i2c(d["bus"])
            for r in d.get("registers", []):
                try:
                    got = lt.i2c_get(t, bus, int(d["addr"]), int(r["reg"]))
                except BenchError as e:
                    bad.append(f"{dev} {int(d['addr']):#04x} reg {int(r['reg']):#04x} {lt.unread(e)}")
                    continue
                mask = int(r.get("mask", 0xFF))
                if got & mask == int(r["expect"]) & mask:
                    continue
                line = f"{dev} {int(d['addr']):#04x} reg {int(r['reg']):#04x} = {got:#04x}, want {int(r['expect']):#04x}"
                if (dev == "act88760" and int(r["reg"]) == lt.ACT88760_GPIO_REG
                        and got == lt.ACT88760_GPIO4_OTP_DEFAULT and otp_default_seen):
                    notes.append(line + " (ACT88760 GPIO4 workaround not yet applied; recorded by census/gd32_flash)")
                else:
                    bad.append(line)
        ctx.step_logs[self.name] = "\n".join(bad + notes)
        if bad:
            raise Refused("register mismatch: " + "; ".join(bad))
        return self.result(ctx, "registers as expected" + (f"; {'; '.join(notes)}" if notes else ""),
                           status="done")


class SecurePage(Step):
    name = "secure_page"

    def probe(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return Unknown("no Linux target")
        bus = ctx.i2c("eeprom")
        page = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.SECURE_PAGE_READ))
        lock = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK_STATUS_READ))[0]
        want = _build_blob(ctx, "program_eeprom_secure_page.py", "secure-page.bin")
        if page != want:
            return Unsatisfied("secure page does not hold this unit's mirror")
        return Satisfied({"secure_page_state": "locked" if lock & LOCK_BIT else "written-verified",
                          "secure_page_sha256": hashlib.sha256(page).hexdigest()})

    def run(self, ctx):
        t = ctx.need_linux()
        bus = ctx.i2c("eeprom")
        if t is not None:
            lock = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK_STATUS_READ))[0]
            if lock & LOCK_BIT:
                raise Refused(f"identity header locked (lock status {lock:#04x}); secure page cannot change")
        blob = _build_blob(ctx, "program_eeprom_secure_page.py", "secure-page.bin")
        ev = {"secure_page_staged_crc32": _crc(blob), "secure_page_staged_sha256": hashlib.sha256(blob).hexdigest()}
        ctx.mutate(f"stage {ctx.serial}.secure-page.staged.bin in the ledger",
                   lambda: _stage(ctx, f"{ctx.serial}.secure-page.staged.bin", blob))
        got = ctx.mutate(f"write the 64-byte Secure Data Page (0x58 selector 0x00) on i2c-{bus}, read back, compare",
                         lambda: lt.secure_page_write_verify(
                             t, bus, gates.identity_frame(gates.IdentityOp.SECURE_PAGE_WRITE, blob),
                             gates.identity_frame(gates.IdentityOp.SECURE_PAGE_READ)))
        if got is not None:
            ev.update(secure_page_state="written-verified", secure_page_sha256=hashlib.sha256(got).hexdigest())
        return self.result(ctx, "secure page written + verified (NOT locked)", ev)


class OpDsw1XspiRemoveSd(Step):
    name = "dsw1_xspi_remove_sd"
    operator = True
    trust_run = True

    def probe(self, ctx):
        if ctx.state_done(self.name):
            return Satisfied({}, "state file: done")
        return Unknown("boot source is checked by cold_boot_test")

    def run(self, ctx):
        b = ctx.need_bench()
        def prompt():
            # Linux still runs from the provisioning SD here: halt it before the operator
            # switches power off or pulls the card
            note = halt_note(clean_shutdown(ctx))
            b.operator.confirm(f"{note} Power OFF, set DSW1 to xSPI boot, REMOVE the microSD. "
                               "Leave it OFF until the tool asks.")
            ctx.halted_on_count = None     # the operator may have powered it back on: never trust the marker now
        ctx.mutate("clean shutdown; operator: set DSW1 to xSPI boot and remove the microSD", prompt)
        return self.result(ctx, "DSW1 on xSPI, microSD removed")


class ColdBootTest(Step):
    name = "cold_boot_test"
    trust_run = True

    def probe(self, ctx):
        if ctx.state_done(self.name):
            return Satisfied({}, "state file: done")
        return Unknown("not run")

    @staticmethod
    def _cold_boot(ctx, ev) -> str:
        return cold_boot_phy_retry(ctx, ev)

    def run(self, ctx):
        n = ctx.cold_cycles
        if n < 1:
            # 0 boots must never satisfy the precondition of the irreversible lock.
            raise Refused(f"cold_cycles must be >= 1 (got {n}): a run with no cold boots proves nothing")
        ev: dict[str, str] = {}
        if not ctx.execute:
            ctx.mutate(f"{n} cold cycles: clean BL2, DRAM tier, "
                       f"{'rail PG, ' if ctx.family == 'v2n-m1' else ''}login, SYS_LSI_MODE, i2c scans",
                       lambda: None)
            return self.result(ctx, f"would run {n} cold cycles")
        expected = {bus: a for bus, a in lt.expected_i2c(ctx.preset, ctx.bench.i2c_bus).items()
                    if bus is not None}
        pmic_bus = ctx.i2c("pmic")
        ev["cold_boots_passed"] = f"0/{n}"
        try:
            return self._cycles(ctx, n, ev, expected, pmic_bus)
        except Exception:
            # a cycle that dies mid-way (console, ssh, i2c) must not drop what the earlier
            # cycles proved: cold_boots_passed says 2/3, and their evidence stays
            ctx.facts.update(ev)
            raise

    def _cycles(self, ctx, n, ev, expected, pmic_bus):
        for i in range(1, n + 1):
            text = ctx.mutate(f"cold cycle {i}/{n}", lambda: self._cold_boot(ctx, ev))
            probs = [f"BL2: {e}" for e in uboot.bl2_errors(text)]
            # firmware versions from THIS cycle's transcript: the ledger takes the last passing
            # cycle's, and a cycle that differs from the one before is a problem
            vers = {**uboot.parse_bl2(text), **uboot.parse_bl31(text)}
            vers.pop("bl2_boot_source", None)
            if v := uboot.parse_uboot_version(text):
                vers["uboot_version"] = v
            probs += [f"{k} changed between cold cycles: {ev[k]!r} -> {v!r}"
                      for k, v in vers.items() if k in ev and ev[k] != v]
            mib = uboot.parse_dram_banner(text)
            tier, tev = tier_gate(ctx, mib)
            if not tier.ok:
                probs.append(tier.detail)
            if mib:
                # boot_sd_linux records these too, but a unit that already
                # boots from eMMC (phase B only) never runs it (#2301).
                ev["dram_size_mib"] = str(mib)
                ev["uboot_dram_banner"] = next(ln.strip() for ln in text.splitlines()
                                               if ln.startswith("DRAM:"))
            if ctx.family == "v2n-m1" and not uboot.has_rail_pg(text):
                probs.append(f"no {uboot.RAIL_PG!r}")
            mode = uboot.parse_sys_lsi(text).get("soc_sys_lsi_mode")
            if mode != SYS_LSI_MODE_XSPI:
                probs.append(f"SYS_LSI_MODE {mode} != {SYS_LSI_MODE_XSPI}")
            # ACT88760 reg 0x10 after this (plain, tool-uninvolved) cold boot:
            # most units' OTP is already 0x08 (production/fixed); an early-OTP
            # unit (OTP 0x88, e.g. E1M-V2M103 2026W38-0001) needs U-Boot's
            # board_late_init to release GD32_NRST every boot -- a no-op on a
            # fixed unit. Only when it is STILL 0x88 here do we exempt the
            # GD32 bridge from the i2c scan (it legitimately stays in reset).
            after = lt.i2c_get(ctx.linux, pmic_bus, lt.ACT88760_ADDR, lt.ACT88760_GPIO_REG)
            ev["act88760_gpio4_after_boot"] = f"{after:#04x}"
            cycle_expected = expected
            if after == lt.ACT88760_GPIO4_OTP_DEFAULT:
                cycle_expected = {bus: a - {GD32_BRIDGE_ADDR} for bus, a in expected.items()}
                ev["cold_boot_note"] = (f"{GD32_BRIDGE_ADDR:#04x} not required: image did not release "
                                        "GD32_NRST (ACT88760 reg 0x10 = 0x88)")
            else:
                # census/gd32_flash having ever seen 0x88 marks this as an
                # early-OTP unit; a clean 0x08 here (a plain cold boot, no
                # provisioning-tool release in it) means U-Boot did it itself.
                # U-Boot 0011's own console line says which case this boot
                # was; fall back to what census/gd32_flash saw this run.
                nrst = uboot.parse_gd32_nrst(text)
                if nrst == "released":
                    ev["act88760_gpio4_otp"] = f"{lt.ACT88760_GPIO4_OTP_DEFAULT:#04x}"
                    ev["act88760_gpio4_workaround"] = "u-boot"
                elif nrst == "already":
                    ev["act88760_gpio4_otp"] = f"{lt.ACT88760_GPIO4_RELEASED:#04x}"
                    ev["act88760_gpio4_workaround"] = "none"
                else:
                    otp_seen_88 = ctx.facts.get("act88760_gpio4_otp") == f"{lt.ACT88760_GPIO4_OTP_DEFAULT:#04x}"
                    ev["act88760_gpio4_workaround"] = "u-boot" if otp_seen_88 else "none"
            probs += lt.i2c_check(ctx.linux, cycle_expected)
            ev.update(tev)
            ev["soc_sys_lsi_mode"] = mode or ""
            ev["cold_boots_passed"] = f"{i - 1 if probs else i}/{n}"
            if probs:
                ctx.facts.update(ev)
                raise Refused(f"cold cycle {i}/{n}: " + "; ".join(probs))
            ev.update(vers)
            if i == 1 and n >= 2 and (functest.config(ctx).get("fixtures") or {}).get("rtc_backup"):
                # functional_test's rtc_retention then has the remaining cold cycles to judge
                ev["rtc_set_boot_id"] = ctx.mutate(
                    "set the RTC from the host clock (fixture rtc_backup: retention is checked after "
                    "the remaining cold cycles)", lambda: functest.rtc_set(ctx.linux))
        if ev.get("cold_boots_passed") != f"{n}/{n}":
            raise Refused(f"cold_boot_test observed {ev.get('cold_boots_passed', '0')} clean boots, want {n}/{n}")
        return self.result(ctx, f"{n}/{n} cold boots clean", ev)


class ClkgenVerify(Step):
    """The on-SoM 5L35023B (BRD_I2C, 0x69) OTP image against U-Boot's
    fixup (U-Boot patch 0007, #2293). Read after the provisioned unit has
    booted: the fixup runs every boot, so a unit shipped without it (or with
    a wrong OTP image) is caught here rather than downstream."""
    name = "clkgen_verify"
    always_run = True

    def run(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return self.result(ctx, "no Linux target: clock-generator verification deferred",
                               status="skipped")
        bus = ctx.i2c("brd")
        image = lt.clkgen_read_image(t, bus)
        bad = lt.clkgen_diff(image)
        line = uboot.parse_clkgen_line(ctx.boot_text)
        if line is None:
            bad.append(f"U-Boot lacks the 5L35023B clkgen fixup (patch 0007, #2293): no "
                       f"{uboot.CLKGEN_LINE_PREFIX!r} line in the last boot console capture")
        ctx.step_logs[self.name] = "\n".join(bad) if bad else f"OTP image ok; boot line: {line!r}"
        if bad:
            raise Refused("5L35023B verification failed: " + "; ".join(bad))
        ev = {"clkgen_otp_raw": " ".join(f"{b:02x}" for b in image),
              "clkgen_i2c_addr": f"{lt.CLKGEN_5L35023B_ADDR:#04x}"}
        return self.result(ctx, f"5L35023B OTP image matches (dash code {image[0x01]:#04x}); "
                           f"boot line: {line!r}", ev, status="done")


class FunctionalTest(Step):
    """Every interface the unit can reach, tested on the unit as shipped: after cold_boot_test's
    last cold boot (xSPI boot, eMMC root). The catalogue, the script and the judges are
    provision/functest.py; the pass criteria are functest-expect-v2n.yaml + --functest-expect.

    A step of its own rather than more of hil_smoke: hil_smoke shells out to the HiL runner
    (built example binaries, one pass/fail for the whole spec directory, no bench.yaml
    fixtures, nothing recorded per check), while this runs through the tool's own Linux
    target in one remote invocation and records one `test_<check>` value per check.

    A check that fails or cannot be read fails the step unless the expected-values file
    lists it `informational`; a fixture the bench does not have is `skipped (no fixture:
    ...)`, which never fails the step. The summary key `test_functional` is what the ship
    check reads."""
    name = "functional_test"
    always_run = True

    def run(self, ctx):
        try:
            x = ctx.functest_expect or functest.load_expect()
            checks = functest.build(ctx, x)
        except (ValueError, KeyError, TypeError, OSError, Refused) as e:
            # preflight checks the same configuration; a run that starts past it still gets a verdict
            return StepResult(self.name, "failed", f"functional test configuration: {e}",
                              {functest.SUMMARY_KEY: lt.unread(e)} if ctx.execute else {})
        fixtures = functest.config(ctx).get("fixtures") or {}
        lanes, wall = functest.estimate(functest.applicable(checks, fixtures))
        budget = "estimated %.0f s (lanes: %s)" % (wall, ", ".join(f"{k} {v:.1f}" for k, v in sorted(lanes.items())))
        if not ctx.execute:
            for c in checks:
                off = c.fixture is not None and not fixtures.get(c.fixture)
                ctx.mutate(f"{c.name}: {c.what}" + (f" [skipped: no fixture {c.fixture}]" if off else ""),
                           lambda: None)
            return self.result(ctx, f"would run {len(checks)} functional checks, {budget}")
        try:
            t = ctx.need_linux()
            values, raw, seconds = functest.run(ctx, t, checks, x)
        except (BenchError, Refused, OSError) as e:
            # no verdict at all is recorded as one: the ship check needs `test_functional: pass`
            return StepResult(self.name, "failed", f"functional test could not run: {e}",
                              {functest.SUMMARY_KEY: lt.unread(e)})
        bad = functest.blocking(values, checks, x)
        info = sorted(n for n, v in values.items() if n not in bad and v.startswith(("fail", "unread")))
        skipped = sorted(n for n, v in values.items() if v.startswith("skipped"))
        # test_ft_<check>: a prefix of its own, so an operator-entered test_<name> is never touched
        ev = {f"{functest.KEY_PREFIX}{n}": v for n, v in values.items()}
        ev.update(functest.side_facts(ctx))
        ev[functest.SUMMARY_KEY] = "pass" if not bad else functest._value("fail", ", ".join(bad))
        ev["functional_test_seconds"] = f"{seconds:.1f}"
        ctx.step_logs[self.name] = "\n".join(
            [f"{n}: {v}" for n, v in values.items()]
            + [f"wall time {seconds:.1f} s; {budget}", "--- script output ---", _elide_long_lines(raw)])
        detail = (f"{sum(v.startswith('pass') for v in values.values())}/{len(values)} passed in {seconds:.1f} s"
                  + (f"; FAILED: {'; '.join(f'{n}: {values[n]}' for n in bad)}" if bad else "")
                  + (f"; informational: {', '.join(info)}" if info else "")
                  + (f"; skipped: {', '.join(skipped)}" if skipped else ""))
        return StepResult(self.name, "failed" if bad else "done", detail, ev)


class HilSmoke(Step):
    name = "hil_smoke"
    trust_run = True

    def probe(self, ctx):
        if ctx.hil_spec is None:
            return Satisfied({}, "no --hil-spec / --carrier: optional step not requested")
        return Unknown("run")

    def run(self, ctx):
        cmd = [sys.executable, str(REPO / "tests" / "hil" / "run_smoke.py"),
               *([] if ctx.execute else ["--validate"]), str(ctx.hil_spec)]
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", check=False,
                              env={**os.environ, "PYTHONIOENCODING": "utf-8"})
        ctx.step_logs[self.name] = proc.stdout + proc.stderr
        if proc.returncode != 0:
            raise Refused(f"run_smoke rc={proc.returncode}")
        ev = {"test_hil_smoke": "pass"} if ctx.execute else {}
        return self.result(ctx, "HiL smoke passed" if ctx.execute else "HiL spec validated", ev)


FLASH_STEPS = ("write_xspi", "write_cm33", "write_emmc_boot", "write_rootfs")
BUNDLE_FACTS = ("bl2_sha256", "rootfs_wic_sha256", "rootfs_bundle_version", "fip_sha256",
                "fip_fdtfile", "fip_rail_string")


class Record(Step):
    name = "record"
    always_run = True

    def run(self, ctx):
        cat_path = ctx.ledger_root / "schema" / "v2n.keys.yaml"
        catalogue = ledger_out.load_catalogue(cat_path)
        unit_yaml = ctx.unit_dir / f"{ctx.serial}.unit.yaml"
        # Steps finished in EARLIER invocations (--only/--from runs) left their facts
        # as evidence in the state file; this run's facts override them.
        facts: dict = {}
        # A tool_rev change (even test-only) moves finished steps to ``superseded``
        # although the unit still carries the SAME bundle's bytes; a step finished
        # there (same bundle_sha256) counts as done. A different bundle's does not.
        # Applies to every step, so a unit provisioned across several tool revisions
        # keeps its facts.
        recovered = {}
        for n in STEP_NAMES:
            cur = ctx.state.get("steps", {}).get(n)
            if cur is not None:
                # The current run's own entry decides; no fallback past a failed/running one.
                if cur.get("status") in ("done", "skipped"):
                    recovered[n] = ("", cur)
                continue
            # The NEWEST superseded group holding an entry for n decides: an older
            # same-bundle write was overwritten by whatever came after it.
            g = next((g for g in reversed(ctx.state.get("superseded", [])) if n in g.get("steps", {})), None)
            if g is None:
                continue
            old = g["steps"][n]
            if g.get("bundle_sha256") == ctx.state.get("bundle_sha256") and old.get("status") in ("done", "skipped"):
                recovered[n] = (f" (step {n} from superseded run, tool_rev {str(g.get('tool_rev'))[:12]}, "
                                f"{old.get('finished') or old.get('ts') or old.get('at') or 'time n/a'})", old)
        flashed = {n: v for n, v in recovered.items() if n in FLASH_STEPS}
        # functional_test's verdict is its LATEST run's. A failed or interrupted run must not
        # leave an earlier `test_functional: pass` standing in the unit record (e.g. `--only
        # functional_test` failed, then `--only record`): its recorded verdict is written, or
        # `unread` when it left none.
        ft = _latest_entry(ctx, FunctionalTest.name)
        if ft is not None and ft.get("status") not in ("done", "skipped"):
            fev = {k: v for k, v in (ft.get("evidence") or {}).items() if k.startswith("test_")}
            if not str(fev.get(functest.SUMMARY_KEY, "")).startswith(("fail", "unread")):
                fev[functest.SUMMARY_KEY] = lt.unread(f"functional_test {ft.get('status')} in its latest run")
            recovered[FunctionalTest.name] = ("", {"evidence": fev})
        # Newest wins: superseded-run facts first, then the current group's, then this run's.
        for note, old in sorted(recovered.values(), key=lambda v: not v[0]):
            facts.update(old.get("evidence") or {})
        facts.update(ctx.facts)
        auto = {k: v for k, v in facts.items()
                if (catalogue.get(k, {}).get("mode") == "auto" or k.startswith("test_")) and v != ""}
        # Bundle facts describe what the bundle CONTAINS; they are ledger facts
        # about the unit only once every flash step succeeded (done, or skipped
        # because the probe found the bundle's bytes already on the unit).
        if len(flashed) < len(FLASH_STEPS):
            for k in BUNDLE_FACTS:
                auto.pop(k, None)
        before = ledger_out.read_unit_yaml(unit_yaml)
        # act88760_gpio4_defect is a legacy key from before the maintainer decision
        # (2026-09-29) that the ACT88760 GPIO4 OTP default is an expected workaround,
        # not a defect; the tool no longer writes it, doesn't block on it (see
        # ledger_out.ship_check) and doesn't hold it sticky -- it is informational
        # only, noted below.
        legacy_defect = before.get("act88760_gpio4_defect", "").strip().lower() == "yes"
        merged = {**before, **auto}
        bench_only = merged.get("rootfs_bundle_version", "").startswith("build-dir:")
        defaults = {"disposition": "bench-only"} if bench_only else {}
        would = [k for k, v in auto.items() if before.get(k) != str(v)] + [k for k in defaults if k not in before]
        changed = ctx.mutate(f"merge {len(would)} key(s) into {unit_yaml.name}: {', '.join(would)}",
                             lambda: ledger_out.merge_unit_yaml(unit_yaml, auto, catalogue, defaults))
        body = "\n".join(f"- `{k}`: {v}" for k, v in sorted(auto.items()))
        for note, _ in recovered.values():
            if note:
                body += f"\n- facts from{note}"
        if legacy_defect:
            body += ("\n- `act88760_gpio4_defect` (legacy `yes`): informational only, no longer "
                     "ship-blocking; see `act88760_gpio4_workaround` / `act88760_gpio4_after_boot`")
        who = f"by {ctx.by}" + (f" at {ctx.station}" if ctx.station else "")
        ctx.mutate(f"append a dated section to {ctx.serial}.md",
                   lambda: ledger_out.append_md_section(ctx.unit_dir / f"{ctx.serial}.md",
                                                        f"provision_som run {who}", body,
                                                        datetime.now(timezone.utc)))
        for step, text in ctx.step_logs.items():
            if text:
                ctx.mutate(f"log {step}", lambda s=step, x=text: ledger_out.write_log(
                    ctx.ledger_root, ctx.sku, ctx.serial, s, x))
        tool = ctx.ledger_xlsx or ctx.ledger_root.parent / "scripts" / "ledger_xlsx.py"
        if tool.is_file():
            def regen():
                p = ledger_out.regen_xlsx(ctx.ledger_root, tool, ctx.ledger_root / "shipped-units.xlsx")
                if p.returncode != 0:
                    raise BenchError(f"ledger_xlsx.py failed: {(p.stderr or p.stdout).strip()}")
            ctx.mutate("regenerate shipped-units.xlsx", regen)
        after = {**before, **{k: str(v) for k, v in auto.items() if catalogue.get(k, {}).get("mode") != "manual"},
                 **{k: v for k, v in defaults.items() if k not in before}}
        blockers = ledger_out.ship_check(after, catalogue, expected_family(ctx.preset))
        detail = f"{len(changed) if changed is not None else len(would)} key(s) " \
                 f"{'updated' if ctx.execute else 'would change'}; ship check: " + \
                 ("SHIPPABLE" if not blockers else "blocked: " + "; ".join(blockers))
        return self.result(ctx, detail, status="done" if ctx.execute else "planned")


class SecurePageLock(Step):
    """`run --lock` only; never in STEP_ORDER."""
    name = "secure_page_lock"

    def preconditions(self, ctx, t) -> list[str]:
        bad = []
        for s in ("secure_page", "cold_boot_test"):
            if not ctx.state_done(s):
                bad.append(f"{s} not done in the state file")
        # Fail closed: the recorded evidence must show >= 1 clean cold boot,
        # all of the requested ones (a done state alone is not proof).
        m = re.fullmatch(r"(\d+)/(\d+)",
                         str(ctx.state.get("steps", {}).get("cold_boot_test", {})
                             .get("evidence", {}).get("cold_boots_passed", "")))
        if ctx.state_done("cold_boot_test") and not (m and int(m[1]) >= 1 and m[1] == m[2]):
            bad.append("cold_boot_test recorded no clean cold boots (cold_boots_passed missing or < 1)")
        bad += [f"{n} failed in the state file"
                for n, v in ctx.state.get("steps", {}).items() if v.get("status") == "failed"]
        bus = ctx.i2c("eeprom")
        written = ctx.unit_dir / f"{ctx.serial}.manifest.bin"
        if not written.is_file():
            bad.append(f"{written.name} not committed in the ledger")
        elif lt.eeprom_read(t, bus, 0, lt.MANIFEST_LEN) != written.read_bytes():
            bad.append(f"array differs from {written.name}")
        lock = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK_STATUS_READ))[0]
        if lock & LOCK_BIT:
            bad.append(f"already locked (lock status {lock:#04x})")
        staged = ctx.unit_dir / f"{ctx.serial}.secure-page.staged.bin"
        page = lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.SECURE_PAGE_READ))
        if not staged.is_file():
            bad.append(f"{staged.name} not in the ledger")
        elif page != staged.read_bytes():
            bad.append(f"secure page differs from {staged.name}")
        unit = ledger_out.read_unit_yaml(ctx.unit_dir / f"{ctx.serial}.unit.yaml")
        catalogue = ledger_out.load_catalogue(ctx.ledger_root / "schema" / "v2n.keys.yaml")
        bad += [f"ship check: {b}" for b in ledger_out.ship_check(unit, catalogue, expected_family(ctx.preset))]
        return bad

    def probe(self, ctx):
        t = ctx.linux
        if t is None:
            return Unknown("no Linux target")
        lock = lt.i2c_transfer(t, ctx.i2c("eeprom"), gates.identity_frame(gates.IdentityOp.LOCK_STATUS_READ))[0]
        return Satisfied({"secure_page_state": "locked", "eeprom_lock_status": f"{lock:#04x}"}) \
            if lock & LOCK_BIT else Unsatisfied(f"lock status {lock:#04x}")

    def run(self, ctx):
        t = ctx.linux
        if t is None:
            raise Refused("no Linux target (set bench.yaml linux.host or boot the unit)")
        bad = self.preconditions(ctx, t)
        if bad:
            raise Refused("lock refused: " + "; ".join(bad))
        bus = ctx.i2c("eeprom")
        if ctx.execute:
            typed = ctx.need_bench().operator.ask(
                f"PERMANENT: lock the N24S128 identity header of {ctx.sku}. Type the unit serial to confirm:")
            if typed != ctx.serial:
                raise Refused(f"operator typed {typed!r}, not {ctx.serial!r}")
        t = ctx.need_linux()                 # fresh identity check right before the irreversible write
        ctx.mutate(f"LOCK: 0x58 frame 04 00 ff on i2c-{bus} (irreversible)",
                   lambda: lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK)))
        return self.result(ctx, "identity header locked" if ctx.execute else "lock plan only (no --execute)")


STEP_ORDER: list[type[Step]] = [
    Preflight, Detect, OpDsw1Scif, Bootstrap, OpDsw1EmmcInsertSd, BootSdLinux, Gd32Flash, WriteXspi,
    WriteCm33, WriteEmmcBoot, WriteRootfs, Census, EepromManifest, Dxm1NpuFlash, PmicVerify,
    SecurePage, OpDsw1XspiRemoveSd, ColdBootTest, CensusFinal, ClkgenVerify, FunctionalTest, HilSmoke, Record,
]
STEP_NAMES = [s.name for s in STEP_ORDER]


# --------------------------------------------------------------------------
# state file + runner
# --------------------------------------------------------------------------

def load_state(path: Path) -> dict:
    path = Path(path)
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_state(path: Path, state: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    os.replace(tmp, path)


def tool_rev() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True,
                              text=True, encoding="utf-8", timeout=10, check=False).stdout.strip() or "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def init_state(ctx: Ctx, bundle_sha256: str) -> None:
    """A state recorded against another bundle or tool revision is evidence
    about OTHER firmware: its steps move to ``superseded`` and every step
    (the unobservable operator / cold-boot ones included) runs again."""
    st = ctx.state
    rev = tool_rev()
    if st.get("steps") and (st.get("bundle_sha256"), st.get("tool_rev")) != (bundle_sha256, rev):
        st.setdefault("superseded", []).append(
            {"bundle_sha256": st.get("bundle_sha256"), "tool_rev": st.get("tool_rev"), "steps": st["steps"]})
        st["steps"] = {}
    st.setdefault("schema", 1)
    st.setdefault("steps", {})
    st.setdefault("overrides", [])
    st.update(sku=ctx.sku, serial=ctx.serial, bundle_sha256=bundle_sha256, tool_rev=rev)


def _safe_probe(step: Step, ctx: Ctx) -> ProbeResult:
    try:
        return step.probe(ctx)
    except (BenchError, Refused, ValueError, OSError) as e:
        return Unknown(f"probe error: {e}")


def run_one(step: Step, ctx: Ctx, force: bool = False) -> StepResult:
    if ctx.bench is None and not ctx.execute and step.name not in ("preflight", "record"):
        return StepResult(step.name, "planned", "offline plan: needs --bench")
    probe = Unknown("forced") if force else (Unknown("always runs") if step.always_run else _safe_probe(step, ctx))
    if isinstance(probe, Satisfied):
        res = StepResult(step.name, "skipped", probe.reason or "already satisfied", probe.evidence)
    else:
        if isinstance(probe, Unsatisfied) and ctx.state_done(step.name):
            ctx.plan_log.append(f"NOTE: {step.name} recorded done but probe says: {probe.reason}; re-running")
        start = len(ctx.plan_log)
        plog = ctx.bench.power.log if ctx.bench is not None else []
        pstart = len(plog)
        tstart = len(ctx.bench.console.transcript) if ctx.bench is not None else 0
        cstart = len(ctx.power_cuts)
        try:
            if ctx.execute and step.name not in ctx.state.get("steps", {}):
                # A run killed mid-step must leave an entry, or Record would fall back
                # to an older same-bundle done entry from a superseded group.
                ctx.state.setdefault("steps", {})[step.name] = {"status": "running", "at": _now()}
                save_state(ctx.state_path, ctx.state)
            res = step.run(ctx)
        except (BenchError, Refused, ValueError, OSError) as e:
            res = StepResult(step.name, "failed", str(e))
        ctx.plan_log.extend(plog[pstart:])  # audit trail: every PSU command, timestamped
        if cuts := ctx.power_cuts[cstart:]:
            # how every power cut of this step was made: clean | fallback | blind | not-needed
            res.evidence["power_cut"] = " | ".join(cuts)
            ctx.step_logs[step.name] = "\n".join(
                filter(None, [ctx.step_logs.get(step.name, ""), *("power cut " + c for c in cuts)]))
        if ctx.bench is not None and ctx.execute:
            # Everything the console said during the step, so a silicon failure is diagnosable.
            seen = _elide_long_lines(_since(ctx.bench.console, tstart))
            ctx.step_logs[step.name] = "\n".join(
                filter(None, [ctx.step_logs.get(step.name, ""), *plog[pstart:],
                              "--- console transcript ---", seen]))
        res.commands = ctx.plan_log[start:]
        if res.status == "done" and not step.always_run:
            post = _safe_probe(step, ctx)
            if isinstance(post, Satisfied):
                res.evidence.update(post.evidence)
            elif not (step.trust_run and isinstance(post, Unknown)):
                res.status = "failed"
                res.detail += f"; post-run probe: {getattr(post, 'reason', post)}"
                if step.name in ctx.step_logs:
                    ctx.step_logs[step.name] += f"\npost-run probe flipped the step to failed: {res.detail}"
        if ctx.bench is not None and ctx.execute and step.name in ctx.step_logs:
            # Written for every outcome (incl. a post-run probe flip): `--only` runs never
            # reach Record, and a failure may end the run early.
            try:
                ledger_out.write_log(ctx.ledger_root, ctx.sku, ctx.serial, step.name, ctx.step_logs[step.name])
            except OSError as e:
                note = f"NOTE: could not write the {step.name} log: {e}"
                ctx.plan_log.append(note)
                res.commands.append(note)
                print(f"provision: could not write the {step.name} log: {e}", file=sys.stderr)
    ctx.facts.update({k: v for k, v in res.evidence.items() if v is not None})
    return res


def select_steps(only: list[str] | None = None, start: str | None = None,
                 skip: list[str] | None = None) -> list[type[Step]]:
    for n in (only or []) + (skip or []) + ([start] if start else []):
        if n not in STEP_NAMES:
            raise ValueError(f"unknown step {n!r}; steps: {', '.join(STEP_NAMES)}")
    out = []
    started = start is None
    for s in STEP_ORDER:
        started = started or s.name == start
        if s.name == "preflight" or started and (not only or s.name in only) and s.name not in (skip or []):        # never skipped
            out.append(s)
    return out


def run_steps(ctx: Ctx, only: list[str] | None = None, start: str | None = None,
              skip: list[str] | None = None, force: list[str] | None = None,
              steps: list[type[Step]] | None = None) -> list[StepResult]:
    results = []
    if ctx.execute:
        ctx.state.setdefault("schema", 1)
        ctx.state.update(sku=ctx.sku, serial=ctx.serial)
    selected = steps or select_steps(only, start, skip)
    for cls in selected:
        res = run_one(cls(), ctx, force=cls.name in (force or []))
        results.append(res)
        if ctx.execute:
            ctx.state.setdefault("steps", {})[res.name] = {
                "status": res.status, "at": _now(), "detail": res.detail, "evidence": res.evidence}
            save_state(ctx.state_path, ctx.state)
        if res.status == "failed":
            # Record still runs: facts learnt so far (the census, a
            # workaround applied) must reach the ledger even when a later
            # step stops the run.
            if Record in selected and cls is not Record:
                results.append(run_one(Record(), ctx))
            break
    return results


def plan_steps(ctx: Ctx) -> list[tuple[str, ProbeResult]]:
    return [(s.name, Unknown("always runs") if s.always_run else _safe_probe(s(), ctx)) for s in STEP_ORDER]
