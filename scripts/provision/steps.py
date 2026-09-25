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
import re
import shlex
import subprocess
import sys
import tempfile
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import date, datetime, timezone
from pathlib import Path
from typing import TypeVar

from provision import gates, ledger_out, uboot
from provision import linux_target as lt
from provision.bench import Bench, BenchError, ExpectTimeout

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
LOCK_BIT = 0x02


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
    allow_tier_mismatch: str | None = None
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
    dxm1_flash: bool = False              # BENCH-PENDING: default skip, see Dxm1NpuFlash
    transfer: str = "sd"                  # "sd" | "xmodem"
    station: str | None = None
    by: str = "provision_som"
    ledger_xlsx: Path | None = None
    boot_text: str = ""                   # last console capture after a power cycle
    boot_class: str = ""
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

    # -- bench ----------------------------------------------------------------
    def need_bench(self) -> Bench:
        if self.bench is None:
            raise Refused("no --bench")
        return self.bench

    def need_linux(self):
        """The Linux target, or None in a dry run without one (plan only)."""
        if self.linux is None and self.execute:
            raise Refused("no Linux target: boot_sd_linux has not run")
        return self.linux

    def linux_up(self) -> bool:
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

def _since(console, n: int) -> str:
    return "".join(console.transcript[n:])


def boot_to_linux(ctx: Ctx, timeout: float = 240.0) -> str:
    """Cold cycle, let the unit autoboot to a login, log in, (re)discover the
    Linux target. Returns the whole boot text. Execute-mode only."""
    b = ctx.need_bench()
    n = len(b.console.transcript)
    b.console.drain()
    b.power.cycle(float(b.raw.get("power", {}).get("off_s", 3.0)))
    b.console.expect(LOGIN_RE, timeout)
    text = _since(b.console, n)
    ctx.boot_text = text
    lt.console_login(b.console, b.linux_user)
    connect_linux(ctx, force=True)
    return text


def connect_linux(ctx: Ctx, force: bool = False) -> None:
    """Attach ctx.linux: bench.yaml linux.host, else discover over the console."""
    if ctx.linux is not None and not force:
        return
    b = ctx.need_bench()
    host = b.linux_host or lt.discover_host(b.console)
    if ctx.linux is not None and getattr(ctx.linux, "host", None) == host:
        return
    ctx.linux = lt.LinuxTarget(host, b.linux_user)


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
        classes = {"scif-rom": scif_writer.ROM_BANNER, "linux-login": LOGIN_RE, "uboot": uboot.PROMPT}
        n = len(c.transcript)
        if ctx.execute:
            ctx.mutate("power cycle and classify the console",
                       lambda: ctx.bench.power.cycle(float(ctx.bench.raw.get("power", {}).get("off_s", 3.0))))
            try:
                key, _ = c.expect_any(classes, 240.0)
            except ExpectTimeout:
                key = None
            text = _since(c, n)
        else:
            text = c.drain()
            key = next((k for k, rx in classes.items() if re.search(rx, text, re.MULTILINE)), None)
        if key is None:
            key = "bl2" if uboot.BL2_VERSION_RE.search(text) else "silent"
        ctx.boot_class, ctx.boot_text = key, text
        ev = {**uboot.parse_bl2(text), **uboot.parse_bl31(text)}
        ev.pop("bl2_boot_source", None)
        if v := uboot.parse_uboot_version(text):
            ev["uboot_version"] = v
        if key == "linux-login" and ctx.execute:
            lt.console_login(c, ctx.bench.linux_user)
            connect_linux(ctx, force=True)
        elif ctx.linux is None and ctx.bench.linux_host:
            ctx.linux = lt.LinuxTarget(ctx.bench.linux_host, ctx.bench.linux_user)
        up = ctx.linux_up()
        return self.result(ctx, f"unit state: {key}; Linux target {'reachable' if up else 'not reachable'}",
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
            c.drain()
            b.power.cycle(float(b.raw.get("power", {}).get("off_s", 3.0)))
            sw.load_writer(c, Path(mot))
        ctx.mutate(f"power cycle; load Flash Writer {Path(mot).name} over SCIF", load)
        ctx.mutate(f"EM_W area {sw.BOOT1_AREA} sector {sw.BL2_MMC_SECTOR:#x}: bl2_mmc ({len(bl2)} bytes)",
                   lambda: sw.em_w(c, sw.BOOT1_AREA, sw.BL2_MMC_SECTOR, ps["bl2_mmc"], bl2))
        ctx.mutate(f"EM_W area {sw.BOOT1_AREA} sector {sw.FIP_SECTOR:#x}: fip ({len(fip)} bytes)",
                   lambda: sw.em_w(c, sw.BOOT1_AREA, sw.FIP_SECTOR, ps["fip"], fip))
        for idx, val in sw.EXT_CSD_WRITES:
            ctx.mutate(f"EM_SECSD EXT_CSD[{idx}] = {val:#04x}", lambda i=idx, v=val: sw.em_secsd(c, i, v))
        ev = ctx.mutate("EM_DCID (read eMMC CID)", lambda: sw.em_dcid(c)) or {}
        return self.result(ctx, "transient bl2_mmc + fip in eMMC boot1, EXT_CSD 177/179 set", ev)


class OpDsw1EmmcInsertSd(_PreLinux):
    name = "dsw1_emmc_insert_sd"
    operator = True

    def probe_console(self, ctx):
        return Satisfied({}, "U-Boot autoboot seen") if re.search(uboot.AUTOBOOT, ctx.boot_text) \
            else Unknown("U-Boot not seen since the last power cycle")

    def run(self, ctx):
        b = ctx.need_bench()
        ctx.mutate("operator: set DSW1 to eMMC boot and insert the release microSD",
                   lambda: b.operator.confirm("Power OFF, set DSW1 to eMMC boot, insert the release microSD."))

        def check():
            n = len(b.console.transcript)
            b.console.drain()
            b.power.cycle(float(b.raw.get("power", {}).get("off_s", 3.0)))
            b.console.expect(uboot.AUTOBOOT, 60.0)
            ctx.boot_text = _since(b.console, n)
        ctx.mutate("cold cycle; expect U-Boot autoboot from eMMC", check)
        return self.result(ctx, "U-Boot boots from eMMC boot1")


class BootSdLinux(Step):
    name = "boot_sd_linux"

    def probe(self, ctx):
        if not ctx.linux_up():
            return Unknown("no Linux target reachable")
        ev = {}
        try:
            ev["root_device"] = lt.root_device(ctx.linux)
        except BenchError:
            pass
        return Satisfied({k: v for k, v in ev.items() if k != "root_device"},
                         f"Linux target reachable (root {ev.get('root_device', '?')})")

    def run(self, ctx):
        b = ctx.need_bench()
        ev: dict[str, str] = {}
        if ctx.transfer == "xmodem":
            addr = (b.raw.get("uboot") or {}).get("load_addr")
            chunk = int((b.raw.get("uboot") or {}).get("gzwrite_chunk") or 16 << 20)
            if addr is None:
                raise Refused("--transfer xmodem needs bench.yaml uboot.load_addr")
            wic = ctx.artefact("system_image")

            def xm():
                text = uboot.cold_to_prompt(b.console, b.power)
                ctx.boot_text = text
                uboot.loadx_gzwrite(b.console, wic, 0, int(addr), chunk)
                n = len(b.console.transcript)
                b.console.send_line("boot")
                b.console.expect(LOGIN_RE, 240.0)
                lt.console_login(b.console, b.linux_user)
                connect_linux(ctx, force=True)
                return text + _since(b.console, n)
            text = ctx.mutate(f"U-Boot loadx + gzwrite {wic.name} into eMMC (slow fallback), boot", xm)
        else:
            text = ctx.mutate("cold cycle; U-Boot bootcmd_check boots the release wic from microSD; "
                              "console login; discover the IPv4 host", lambda: boot_to_linux(ctx))
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
        t.run("true")
        if ctx.transfer == "sd":
            root, emmc = lt.root_device(t), lt.resolve_emmc(t)
            if root.startswith(emmc):
                raise Refused(f"Linux root {root} is on the eMMC, not the microSD "
                              "(SDHI1 not up? SD mux? check U-Boot patch 0007)")
        return self.result(ctx, f"Linux up on {getattr(t, 'host', '?')}", ev)


class WriteXspi(Step):
    name = "write_xspi"

    def probe(self, ctx):
        t = ctx.linux
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
        for mtd, role, limit in ((0, "bl2", None), (1, "fip", gates.CM33_REGION_OFFSET)):
            p = ctx.artefact(role)
            ctx.mutate(f"mtd{mtd} <- {role} {p.name} ({p.stat().st_size} bytes): "
                       "flash_erase, mtd_debug write, md5 readback",
                       lambda m=mtd, q=p, lim=limit: lt.mtd_write_verify(t, m, q, lim))
        return self.result(ctx, "bl2 -> mtd0, fip -> mtd1 (CM33 region untouched)")


class WriteEmmcBoot(Step):
    name = "write_emmc_boot"

    def probe(self, ctx):
        t = ctx.linux
        if t is None:
            return Unknown("no Linux target")
        emmc = lt.resolve_emmc(t)
        ev = {}
        for key, role, sector in (("emmc_boot1_bl2_md5", "bl2_mmc", gates.BL2_MMC_SECTOR),
                                  ("emmc_boot1_fip_md5", "fip", gates.FIP_SECTOR)):
            data = ctx.artefact_bytes(role)
            got = t.md5(f"{emmc}boot1", sector * 512, len(data))
            if got != _md5(data):
                return Unsatisfied(f"{emmc}boot1 sector {sector:#x} != bundle {role}")
            ev[key] = got
        regs = lt.ext_csd(t, emmc)
        if regs[177] != 0x02 or regs[179] != 0x08:
            return Unsatisfied(f"EXT_CSD [177]={regs[177]:#04x} [179]={regs[179]:#04x}")
        ev.update(emmc_ext_csd_177="0x02", emmc_ext_csd_179="0x08")
        return Satisfied(ev)

    def run(self, ctx):
        t = ctx.need_linux()
        emmc = lt.resolve_emmc(t) if t else "/dev/<emmc>"
        for role, sector in (("bl2_mmc", gates.BL2_MMC_SECTOR), ("fip", gates.FIP_SECTOR)):
            p = ctx.artefact(role)
            ctx.mutate(f"{emmc}boot1 sector {sector:#x} <- {role} {p.name} (force_ro cleared for the write)",
                       lambda q=p, s=sector: lt.emmc_boot1_write_verify(t, emmc, q, s))
        ctx.mutate(f"mmc-utils on {emmc}: EXT_CSD[177]=0x02, [179]=0x08",
                   lambda: lt.set_boot_config(t, emmc))
        return self.result(ctx, "release bl2_mmc + fip in eMMC boot1; boot config set")


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


class WriteRootfs(Step):
    name = "write_rootfs"

    def probe(self, ctx):
        t = ctx.linux
        if t is None:
            return Unknown("no Linux target")
        emmc = lt.resolve_emmc(t)
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
        ctx.mutate(f"gunzip -c {wic.name} | dd of={emmc} bs=4M conv=fsync (over SSH), md5 readback",
                   lambda: lt.rootfs_write_verify(t, emmc, wic))

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
        part = ctx.mutate(f"fsck -n, mount ro, /boot/{dtb} present", check)
        return self.result(ctx, f"wic written; rootfs p{part} holds /boot/{dtb}" if part
                           else "would write the wic and check the rootfs")


class Census(Step):
    name = "census"
    always_run = True

    def run(self, ctx):
        t = ctx.need_linux()
        if t is None:
            return self.result(ctx, "no Linux target: census deferred", status="skipped")
        bus = {k: ctx.bench.i2c_bus.get(k) if ctx.bench else None for k in ("eeprom", "pmic", "brd")}
        bus["eeprom"] = bus["eeprom"] if bus["eeprom"] is not None else 0
        sizes = {r: len(ctx.artefact_bytes(r)) for r in ("bl2", "fip", "bl2_mmc")
                 if any(c.get("role") == r for c in ctx.bundle.get("components", []))}
        facts, notes = lt.census(t, bus, sizes)
        if bus["pmic"] is not None:
            try:
                if lt.act88760_gpio4_defect(t, bus["pmic"]):
                    facts["act88760_gpio4_defect"] = "yes"   # "no" is never inferred from a live read
            except BenchError as e:
                notes.append(f"act88760_gpio4_defect: {e}")
        ctx.step_logs[self.name] = "\n".join(notes)
        return self.result(ctx, f"{len(facts)} keys" + (f"; unread: {'; '.join(notes)}" if notes else ""),
                           facts, status="done")


class EepromManifest(Step):
    name = "eeprom_manifest"

    def probe(self, ctx):
        t = ctx.linux
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
            r = t.run(f"head -c {gates.CM33_REGION_OFFSET} /dev/mtd1 | grep -q -a -F "
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
            boot_to_linux(ctx)
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

    def _readback(self, ctx, images) -> dict[str, str]:
        ev = {}
        with tempfile.TemporaryDirectory(prefix="gd32_") as td:
            for i, (p, addr, key) in enumerate(images):
                out = Path(td) / f"rb{i}.bin"
                ctx.bench.probe.savebin(out, addr, p.stat().st_size)   # fresh session each
                ev[key] = _md5(out.read_bytes())
        return ev

    def probe(self, ctx):
        if ctx.bench is None or ctx.bench.probe is None or ctx.gd32_fw is None:
            return Unknown("no probe or no --gd32-fw")
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
        return Satisfied(ev)

    def run(self, ctx):
        ev: dict[str, str] = {}
        t = ctx.need_linux()
        if t is not None:
            pmic = ctx.i2c("pmic")
            if lt.act88760_gpio4_defect(t, pmic):
                ev["act88760_gpio4_defect"] = "yes"
                ev["act88760_gpio4_workaround"] = "volatile 0x08"
                ctx.mutate("ACT88760 0x25 reg 0x10 = 0x08 (volatile GPIO4 / GD32_NRST release)",
                           lambda: lt.act88760_gpio4_release(t, pmic))
        b = ctx.need_bench()
        if b.probe is None:
            raise Refused("bench.yaml has no probe: this bench has no SWD path to the GD32")
        images = self._images(ctx)
        dp = b.probe.dp_id()
        ev["gd32_dp_id"] = f"0x{dp:08x}"
        if dp != GD32_DP_OK:
            why = GD32_DP_REFUSE.get(dp, "an unknown debug port")
            raise Refused(f"DP-ID {dp:#010x} is {why}; want {GD32_DP_OK:#010x}")
        for p, addr, _key in images:
            ctx.mutate(f"loadbin {p.name} @ {addr:#010x}", lambda q=p, a=addr: b.probe.loadbin(q, a))

        def verify():
            got = self._readback(ctx, images)
            for p, _a, key in images:
                if got[key] != _md5(p.read_bytes()):
                    raise BenchError(f"GD32 readback of {p.name} does not match")
            return got
        ev.update(ctx.mutate("verify each region with savebin in a FRESH probe session, md5", verify) or {})
        ctx.mutate("reset/run the GD32", b.probe.reset_run)

        def bridge():
            r = t.run("dmesg | grep 'GD32 bridge protocol' | tail -n1", check=False).stdout.strip()
            if not r:
                raise BenchError("dmesg shows no 'GD32 bridge protocol' line")
            if GD32_BRIDGE_ADDR not in lt.i2c_scan(t, ctx.i2c("brd")):
                raise BenchError(f"GD32 bridge does not ACK at {GD32_BRIDGE_ADDR:#04x}")
            return r
        line = ctx.mutate("check dmesg 'GD32 bridge protocol' and the 0x70 ACK", bridge)
        if line:
            ev["gd32_protocol"] = line
        return self.result(ctx, "GD32 flashed and verified" if ctx.execute else "would flash the GD32", ev)


DXM1_REFUSED_GPIO_LINES = {52: "P64", 53: "P65"}   # the DEEPX 0.75 V rail; never a UART-mux/reset line


class Dxm1NpuFlash(Step):
    """BENCH-PENDING: DX-M1 SPI-NAND-over-UART recovery has never run on
    silicon and cannot succeed on the first V2M bench unit yet (its DX-M1
    does not start its reference clock). Skipped unless the caller passes
    --enable-dxm1-flash, and only for a v2n-m1 (DEEPX) family bundle. The
    DX-M1 must be strapped for SPI-NAND/UART boot per the internal hardware
    notes before this step can run for real."""
    name = "dxm1_npu_flash"
    always_run = True

    def run(self, ctx):
        if ctx.family != "v2n-m1":
            return self.result(ctx, "not a V2M/DEEPX SKU: no DX-M1 to flash", status="skipped")
        if not ctx.dxm1_flash:
            return self.result(
                ctx, "BENCH-PENDING: DX-M1 SPI-NAND-over-UART recovery has never run on silicon "
                "and cannot succeed on the first V2M bench unit yet (its DX-M1 does not start its "
                "reference clock); pass --enable-dxm1-flash once bench-verified",
                status="skipped")
        b = ctx.need_bench()
        d = b.raw.get("dxm1") or {}
        for key in ("gpio_chip", "uart_mux_line", "reset_line", "uart_device",
                    "uart_boot", "fw_uart_boot", "fw"):
            if d.get(key) is None:
                raise Refused(f"bench.yaml dxm1.{key} is TBD (null)")
        for key in ("uart_mux_line", "reset_line"):
            line = d[key]
            if not isinstance(line, int) or isinstance(line, bool):
                raise Refused(f"bench.yaml dxm1.{key} must be an int gpiochip line number, got {line!r}")
            if line in DXM1_REFUSED_GPIO_LINES:
                raise Refused(f"bench.yaml dxm1.{key} = {line} is {DXM1_REFUSED_GPIO_LINES[line]} "
                              "(the DEEPX 0.75 V rail): gpiolib reconfigures a pin on read and would "
                              "kill the rail; refusing")
        if d["uart_mux_line"] == d["reset_line"]:
            raise Refused(f"bench.yaml dxm1.uart_mux_line == dxm1.reset_line ({d['uart_mux_line']}); "
                          "these must be distinct gpiochip lines")
        fw_boot_local, fw_local = Path(d["fw_uart_boot"]), Path(d["fw"])
        uart_boot_local = Path(d["uart_boot"])
        for path, want, label in ((fw_boot_local, lt.DXM1_FW_UART_BOOT_MD5, "fw_uart_boot"),
                                  (fw_local, lt.DXM1_FW_MD5, "fw"),
                                  (uart_boot_local, lt.DXM1_UART_BOOT_TOOL_MD5, "uart_boot")):
            got = _md5(path.read_bytes())
            if got != want:
                raise Refused(f"{label} {path.name}: md5 {got} != pinned {want} "
                              f"(want DEEPX release {lt.DXM1_FW_VERSION})")
        t = ctx.need_linux()
        remote_tool, remote_boot, remote_fw = "/tmp/uart_boot", "/tmp/fw_uart_boot.bin", "/tmp/fw.bin"

        def flash():
            t.put(Path(d["uart_boot"]), remote_tool)
            t.put(fw_boot_local, remote_boot)
            t.put(fw_local, remote_fw)
            t.run(f"chmod +x {remote_tool}")
            return lt.dxm1_uart_boot(t, d["gpio_chip"], d["uart_mux_line"], d["reset_line"],
                                     d["uart_device"], remote_tool, remote_boot, remote_fw)
        ctx.mutate(f"hold P75 high, pulse PA6, run vendor uart_boot (bootloader then "
                   f"firmware {lt.DXM1_FW_VERSION}), release P75", flash)

        def verify():
            boot_to_linux(ctx)
            if not lt.dxm1_pcie_present(ctx.linux, d.get("pcie_vendor_id")):
                raise Refused("cold boot: /sys/bus/pci/devices shows no DEEPX endpoint "
                              "(root port only, or none at all); no DEEPX PCIe link")
        ctx.mutate("cold boot; verify a DEEPX PCIe endpoint enumerates (not just the root port)", verify)
        ev = {"dxm1_fw_uart_boot_md5": lt.DXM1_FW_UART_BOOT_MD5, "dxm1_fw_md5": lt.DXM1_FW_MD5,
              "dxm1_fw_version": lt.DXM1_FW_VERSION, "dxm1_uart_boot_tool_md5": lt.DXM1_UART_BOOT_TOOL_MD5}
        return self.result(ctx, "DX-M1 SPI-NAND programmed over UART recovery and verified "
                           "(PCIe link up)" if ctx.execute
                           else "would program the DX-M1 over UART recovery", ev)


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
        defect = ctx.facts.get("act88760_gpio4_defect") == "yes"
        bad, notes = [], []
        for dev, d in (spec.get("devices") or {}).items():
            fams = d.get("families")
            if fams and ctx.family not in fams:
                continue
            bus = ctx.i2c(d["bus"])
            for r in d.get("registers", []):
                got = lt.i2c_get(t, bus, int(d["addr"]), int(r["reg"]))
                mask = int(r.get("mask", 0xFF))
                if got & mask == int(r["expect"]) & mask:
                    continue
                line = f"{dev} {int(d['addr']):#04x} reg {int(r['reg']):#04x} = {got:#04x}, want {int(r['expect']):#04x}"
                if dev == "act88760" and int(r["reg"]) == lt.ACT88760_GPIO_REG and got == lt.ACT88760_GPIO4_DEFECT and defect:
                    notes.append(line + " (known GPIO4 defect, recorded by gd32_flash)")
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
        t = ctx.linux
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
        ctx.mutate("operator: set DSW1 to xSPI boot and remove the microSD",
                   lambda: b.operator.confirm("Power OFF, set DSW1 to xSPI boot, REMOVE the microSD."))
        return self.result(ctx, "DSW1 on xSPI, microSD removed")


class ColdBootTest(Step):
    name = "cold_boot_test"
    trust_run = True

    def probe(self, ctx):
        if ctx.state_done(self.name):
            return Satisfied({}, "state file: done")
        return Unknown("not run")

    def run(self, ctx):
        n = ctx.cold_cycles
        ev: dict[str, str] = {}
        if not ctx.execute:
            ctx.mutate(f"{n} cold cycles: clean BL2, DRAM tier, "
                       f"{'rail PG, ' if ctx.family == 'v2n-m1' else ''}login, SYS_LSI_MODE, i2c scans",
                       lambda: None)
            return self.result(ctx, f"would run {n} cold cycles")
        expected = {bus: a for bus, a in lt.expected_i2c(ctx.preset, ctx.bench.i2c_bus).items()
                    if bus is not None}
        if ctx.facts.get("act88760_gpio4_defect") == "yes":
            # the volatile release is lost at every power-off: the GD32 stays in reset
            expected = {bus: a - {GD32_BRIDGE_ADDR} for bus, a in expected.items()}
            ev["cold_boot_note"] = f"{GD32_BRIDGE_ADDR:#04x} not required: act88760_gpio4_defect recorded"
        for i in range(1, n + 1):
            text = ctx.mutate(f"cold cycle {i}/{n}", lambda: boot_to_linux(ctx))
            probs = [f"BL2: {e}" for e in uboot.bl2_errors(text)]
            tier, tev = tier_gate(ctx, uboot.parse_dram_banner(text))
            if not tier.ok:
                probs.append(tier.detail)
            if ctx.family == "v2n-m1" and not uboot.has_rail_pg(text):
                probs.append(f"no {uboot.RAIL_PG!r}")
            mode = uboot.parse_sys_lsi(text).get("soc_sys_lsi_mode")
            if mode != SYS_LSI_MODE_XSPI:
                probs.append(f"SYS_LSI_MODE {mode} != {SYS_LSI_MODE_XSPI}")
            probs += lt.i2c_check(ctx.linux, expected)
            ev.update(tev)
            ev["soc_sys_lsi_mode"] = mode or ""
            ev["cold_boots_passed"] = f"{i - 1 if probs else i}/{n}"
            if probs:
                ctx.facts.update(ev)
                raise Refused(f"cold cycle {i}/{n}: " + "; ".join(probs))
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


class Record(Step):
    name = "record"
    always_run = True

    def run(self, ctx):
        cat_path = ctx.ledger_root / "schema" / "v2n.keys.yaml"
        catalogue = ledger_out.load_catalogue(cat_path)
        unit_yaml = ctx.unit_dir / f"{ctx.serial}.unit.yaml"
        auto = {k: v for k, v in ctx.facts.items()
                if (catalogue.get(k, {}).get("mode") == "auto" or k.startswith("test_")) and v != ""}
        before = ledger_out.read_unit_yaml(unit_yaml)
        if before.get("act88760_gpio4_defect", "").lower() == "yes":   # sticky: never auto-downgraded
            auto.pop("act88760_gpio4_defect", None)
            auto.pop("act88760_gpio4_workaround", None)
        merged = {**before, **auto}
        bench_only = (merged.get("act88760_gpio4_defect", "").lower() == "yes"
                      or merged.get("rootfs_bundle_version", "").startswith("build-dir:"))
        defaults = {"disposition": "bench-only"} if bench_only else {}
        would = [k for k, v in auto.items() if before.get(k) != str(v)] + [k for k in defaults if k not in before]
        changed = ctx.mutate(f"merge {len(would)} key(s) into {unit_yaml.name}: {', '.join(would)}",
                             lambda: ledger_out.merge_unit_yaml(unit_yaml, auto, catalogue, defaults))
        body = "\n".join(f"- `{k}`: {v}" for k, v in sorted(auto.items()))
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
        blockers = ledger_out.ship_check(after, catalogue)
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
        bad += [f"ship check: {b}" for b in ledger_out.ship_check(unit, catalogue)]
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
        ctx.mutate(f"LOCK: 0x58 frame 04 00 ff on i2c-{bus} (irreversible)",
                   lambda: lt.i2c_transfer(t, bus, gates.identity_frame(gates.IdentityOp.LOCK)))
        return self.result(ctx, "identity header locked" if ctx.execute else "lock plan only (no --execute)")


STEP_ORDER: list[type[Step]] = [
    Preflight, Detect, OpDsw1Scif, Bootstrap, OpDsw1EmmcInsertSd, BootSdLinux, WriteXspi,
    WriteEmmcBoot, WriteRootfs, Census, EepromManifest, Gd32Flash, Dxm1NpuFlash, PmicVerify,
    SecurePage, OpDsw1XspiRemoveSd, ColdBootTest, ClkgenVerify, HilSmoke, Record,
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
        try:
            res = step.run(ctx)
        except (BenchError, Refused, ValueError, OSError) as e:
            res = StepResult(step.name, "failed", str(e))
        res.commands = ctx.plan_log[start:]
        if res.status == "done" and not step.always_run:
            post = _safe_probe(step, ctx)
            if isinstance(post, Satisfied):
                res.evidence.update(post.evidence)
            elif not (step.trust_run and isinstance(post, Unknown)):
                res.status = "failed"
                res.detail += f"; post-run probe: {getattr(post, 'reason', post)}"
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
            # Record still runs: facts learnt so far (a defect, the census)
            # must reach the ledger even when a later step stops the run.
            if Record in selected and cls is not Record:
                results.append(run_one(Record(), ctx))
            break
    return results


def plan_steps(ctx: Ctx) -> list[tuple[str, ProbeResult]]:
    return [(s.name, Unknown("always runs") if s.always_run else _safe_probe(s(), ctx)) for s in STEP_ORDER]
