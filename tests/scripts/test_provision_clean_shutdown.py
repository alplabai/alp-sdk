"""Never hard-cut power under a running Linux (#2624): clean_shutdown() before every tool-driven
power cut, its outcome record, and the census supply-current screen. Fakes only, no bench."""

from __future__ import annotations

import re

import pytest
from provision import linux_target as lt
from provision import steps
from provision.bench import BenchError

from .provision_fakes import FakeConsole, FakeLinux, FakeOperator, FakePower
from .test_provision_steps import Board, _bench, _ctx

NONCE = "abc12345"
HALT = "reboot: Power down\r\n"
SYNC_OK = f"ALPS{NONCE}:0\r\n"
# the one console line a console-path shutdown sends (marker split by "" so its echo cannot match)
POWEROFF_LINE = r'^sync; echo "ALPS""abc12345:\$\?"; poweroff\r$'


@pytest.fixture(autouse=True)
def fixed_nonce(monkeypatch):
    monkeypatch.setattr(steps, "_nonce", lambda: NONCE)


class LoggedConsole(FakeConsole):
    """A scripted console that records every write into the shared `order` list."""

    def __init__(self, script, order, chunk=None):
        super().__init__(script, chunk=chunk)
        self.order = order

    def _write_raw(self, data):
        self.order.append("console:" + data.decode("latin-1").strip())
        super()._write_raw(data)


class OrderedPower(FakePower):
    """FakePower that logs into the shared `order` list, so a test sees poweroff come before off."""

    def __init__(self, order, on_hook=None, state=None):
        super().__init__(on_hook=on_hook, state=state)
        self.order = order

    def off(self):
        self.order.append("psu-off")
        super().off()


class OrderedOperator(FakeOperator):
    def __init__(self, order):
        super().__init__()
        self._say = lambda message: order.append("confirm: " + message)


class Unit(FakeLinux):
    """An SSH-reachable unit. `same` = it is the unit on OUR console (its nonce shows up there);
    `halts` = the halt line it prints on poweroff (True = the usual one, False/"" = none);
    `late` = the plain `sync` prints it instead (a slow halt)."""

    def __init__(self, console, order, same=True, halts=True, late=False, sync_rc=0, first_rc=0):
        super().__init__()
        self.console, self.order, self.same, self.halts, self.late, self.sync_rc, self.first_rc = (
            console, order, same, halts, late, sync_rc, first_rc)

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None, long_running=False):
        if cmd == "true":
            self.order.append("true")
            return lt.CmdResult(0, "", "")
        if cmd.startswith("echo ALPID"):
            self.order.append("ident")
            if self.same:
                self.console.feed(re.search(r"ALPID(\w+)", cmd).group(0))
            return lt.CmdResult(0, "", "")
        if cmd.startswith("sync; echo ALPSYNC:$?; poweroff"):
            self.order.append("poweroff")
            if self.halts and not self.late:
                self.console.feed(HALT if self.halts is True else self.halts)
            return lt.CmdResult(0, f"ALPSYNC:{self.first_rc}\n", "")
        if cmd == "sync":
            self.order.append("sync")
            if self.late:
                self.console.feed(HALT)
            if self.sync_rc == "raise":
                raise BenchError("ssh: connection refused")
            return lt.CmdResult(self.sync_rc, "", "")
        if check:
            raise BenchError(f"{cmd!r}: unscripted")
        return lt.CmdResult(1, "", "unscripted")


def _setup(tmp_path, ssh=True, script=(), power_state=None, **unit):
    order: list[str] = []
    console = LoggedConsole(list(script), order)
    b = _bench(console=console)
    b.power = OrderedPower(order, on_hook=lambda: console.feed("login: "), state=power_state)
    b.operator = OrderedOperator(order)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    if ssh:
        ctx.linux = Unit(console, order, **unit)
    return ctx, order


def _poweroff_over_console(then=HALT):
    return [(POWEROFF_LINE, SYNC_OK + then)]


# --- outcome: clean ----------------------------------------------------------------------------

def test_ssh_poweroff_comes_after_the_identity_proof_and_before_the_psu_is_cut(tmp_path):
    ctx, order = _setup(tmp_path)
    assert steps.clean_shutdown(ctx) == "clean"
    ctx.bench.power.cycle(15.0, ctx.bench.console)
    assert order == ["true", "ident", "poweroff", "psu-off"]
    assert ctx.power_cuts == ["clean: ssh poweroff, halt line seen; sync rc=0"]
    assert ctx.plan_log[0] == "power cut " + ctx.power_cuts[0]


def test_system_halted_is_also_a_halt_line(tmp_path):
    ctx, _ = _setup(tmp_path, halts="System halted.\r\n")
    assert steps.clean_shutdown(ctx) == "clean"


def test_a_halt_line_that_arrives_late_still_counts_as_clean(tmp_path):
    ctx, order = _setup(tmp_path, late=True)
    assert steps.clean_shutdown(ctx) == "clean"
    assert order[-2:] == ["poweroff", "sync"] and "(late)" in ctx.power_cuts[0]


# --- outcome: fallback -------------------------------------------------------------------------

def test_no_halt_line_over_ssh_records_a_fallback_with_what_the_second_sync_did(tmp_path):
    ctx, order = _setup(tmp_path, halts=False)
    assert steps.clean_shutdown(ctx) == "fallback"
    assert order == ["true", "ident", "poweroff", "sync"]
    assert re.fullmatch(r"fallback: ssh: no halt line in .*; first sync rc=0; second sync rc=0", ctx.power_cuts[0])


def test_a_failed_fallback_sync_is_not_reported_as_a_sync(tmp_path):
    ctx, _ = _setup(tmp_path, halts=False, sync_rc="raise")
    steps.clean_shutdown(ctx)
    assert "second sync raised: ssh: connection refused" in ctx.power_cuts[0]
    ctx2, _ = _setup(tmp_path / "b", halts=False, sync_rc=1)
    steps.clean_shutdown(ctx2)
    assert "second sync rc=1" in ctx2.power_cuts[0]


def test_the_console_path_never_sends_a_second_command(tmp_path):
    """A second command through ConsoleTarget would Ctrl-C the running `sync; poweroff`."""
    ctx, order = _setup(tmp_path, ssh=False, script=_poweroff_over_console(then=""))
    ctx.console_login_on_count = ctx.bench.power.on_count
    assert steps.clean_shutdown(ctx) == "fallback"
    assert [o for o in order if o.startswith("console:")] == ['console:sync; echo "ALPS""abc12345:$?"; poweroff']
    assert not any("\x03" in w for w in ctx.bench.console.written)
    assert "first sync rc=0; no second command sent on the console" in ctx.power_cuts[0]


# --- paths -------------------------------------------------------------------------------------

def test_this_boots_console_login_is_preferred_over_ssh(tmp_path):
    ctx, order = _setup(tmp_path, script=_poweroff_over_console())
    ctx.console_login_on_count = ctx.bench.power.on_count
    assert steps.clean_shutdown(ctx) == "clean"
    assert "ident" not in order and "poweroff" not in order and "true" not in order   # ssh untouched
    assert ctx.power_cuts == ["clean: console poweroff, halt line seen; sync rc=0"]


def test_an_ssh_host_that_is_not_on_our_console_is_not_halted(tmp_path):
    """A stale lease points at ANOTHER unit: it must not get `poweroff`, and ours is a blind cut."""
    ctx, order = _setup(tmp_path, same=False)
    assert steps.clean_shutdown(ctx) == "blind"
    assert order == ["true", "ident"]
    assert "did not echo this unit's console nonce" in ctx.power_cuts[0]


def test_a_stale_console_login_from_an_earlier_boot_is_not_used(tmp_path):
    ctx, order = _setup(tmp_path, ssh=False)
    ctx.console_login_on_count = ctx.bench.power.on_count - 1      # an ON happened since
    assert steps.clean_shutdown(ctx) == "blind"
    assert order == [] and "console_login" not in str(ctx.power_cuts)


def test_a_pinned_host_from_a_previous_run_is_halted_too(tmp_path, monkeypatch):
    """Detect starts a fresh process on a board that is still up: the pinned bench.yaml host is
    the only way to know, and the hard cut it used to get is what corrupted the SD root."""
    ctx, order = _setup(tmp_path, ssh=False)
    ctx.bench.linux_host = "192.0.2.7"
    monkeypatch.setattr(lt, "LinuxTarget", lambda host, user: Unit(ctx.bench.console, order))
    assert steps.clean_shutdown(ctx) == "clean"
    assert order == ["true", "ident", "poweroff"]


# --- the unknown-console probe -----------------------------------------------------------------

def test_the_probe_sends_ctrl_c_only_and_leaves_a_uboot_prompt_alone(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    ctx, order = _setup(tmp_path, ssh=False, script=[(r"^\x03$", "\r\n=> ")])
    assert steps.clean_shutdown(ctx) == "not-needed"
    assert ctx.bench.console.written == ["\x03"]                    # no bare Enter: U-Boot would repeat a command
    assert ctx.power_cuts == ["not-needed: U-Boot prompt, no shutdown needed"]


def test_the_probe_finds_a_linux_shell_and_halts_through_it(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    logins = []
    monkeypatch.setattr(lt, "console_login", lambda c, u, timeout=120.0: logins.append(timeout))
    ctx, order = _setup(tmp_path, ssh=False, script=[(r"^\x03$", "root@board:~# ")] + _poweroff_over_console())
    assert steps.clean_shutdown(ctx) == "clean"
    assert logins == [5.0] and order[0] == "console:\x03" and order[1].startswith("console:sync; echo")
    assert ctx.power_cuts == ["clean: console poweroff, halt line seen; sync rc=0"]


def test_a_console_that_answers_nothing_is_a_blind_cut_not_a_skip(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)

    def no_shell(c, u, timeout=120.0):
        raise BenchError("no login prompt")
    monkeypatch.setattr(lt, "console_login", no_shell)
    ctx, order = _setup(tmp_path, ssh=False, script=[(r"^\x03$", "")])
    assert steps.clean_shutdown(ctx) == "blind"
    assert "if Linux is up this cut is hard" in ctx.power_cuts[0]


def test_nothing_is_sent_to_the_scif_rom_or_flash_writer(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    ctx, order = _setup(tmp_path, ssh=False)
    ctx.rom_console_on_count = ctx.bench.power.on_count
    assert steps.clean_shutdown(ctx) == "not-needed"
    assert order == [] and ctx.bench.console.written == []


# --- not-needed / dry run / once per power-on ---------------------------------------------------

def test_a_unit_that_is_already_off_needs_no_shutdown(tmp_path):
    ctx, order = _setup(tmp_path, power_state=False)
    assert steps.clean_shutdown(ctx) == "not-needed" and order == []


def test_a_second_call_on_the_same_power_on_does_nothing(tmp_path):
    """dsw1_xspi_remove_sd halts the unit; cold_boot_test's first cycle must not type into it."""
    ctx, order = _setup(tmp_path, script=_poweroff_over_console())
    ctx.console_login_on_count = ctx.bench.power.on_count
    assert steps.clean_shutdown(ctx) == "clean"
    assert ctx.console_login_on_count is None
    n = len(order)
    assert steps.clean_shutdown(ctx) == "not-needed" and len(order) == n
    assert ctx.power_cuts[-1] == "not-needed: already halted on this power-on"


def test_a_dry_run_never_touches_the_unit(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.execute = False
    assert steps.clean_shutdown(ctx) == "" and order == [] and ctx.power_cuts == []


# --- every cut site (removing a call must break one of these) -------------------------------------

def _boot_patches(monkeypatch, order):
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: None)

    def connect(c, force=False, rediscover=False):
        c.linux = Unit(c.bench.console, order)
    monkeypatch.setattr(steps, "connect_linux", connect)


HALTED = ["true", "ident", "poweroff", "psu-off"]


def test_boot_to_linux_halts_before_every_cold_cycle_and_keeps_the_off_dwell(tmp_path, monkeypatch):
    """cold_boot_test = 3 x cold_boot_phy_retry -> boot_to_linux: three clean poweroffs, three cuts
    (the first over ssh, the later ones through the console login the boot before left)."""
    ctx, order = _setup(tmp_path, script=_poweroff_over_console() * 2)
    _boot_patches(monkeypatch, order)
    for _ in range(3):
        steps.boot_to_linux(ctx)
    kinds = [("off" if o == "psu-off" else "halt") for o in order
             if o in ("poweroff", "psu-off") or o.startswith("console:sync; echo")]
    assert kinds == ["halt", "off"] * 3
    assert ctx.bench.power.events == ["off", "on"] * 3
    assert [c.split(":")[0] for c in ctx.power_cuts] == ["clean"] * 3


def test_resuming_a_running_boot_does_not_power_off_the_unit(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path)
    _boot_patches(monkeypatch, order)
    ctx.bench.console.feed("login: ")
    steps.boot_to_linux(ctx, resume_mark=0)
    assert order == [] and ctx.bench.power.events == []


def test_poweroff_and_cold_boot_is_a_clean_shutdown_then_a_cold_boot(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path)
    _boot_patches(monkeypatch, order)
    steps.poweroff_and_cold_boot(ctx)
    assert order[:4] == HALTED and ctx.bench.power.events == ["off", "on"]


def test_detect_halts_a_running_linux_before_the_power_cycle(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.bench.power.on_hook = lambda: ctx.bench.console.feed("=> ")
    steps.Detect().run(ctx)
    assert order[:4] == HALTED


def test_bootstrap_halts_a_running_linux_before_its_power_cycle(tmp_path, monkeypatch):
    from provision import scif_writer as sw
    ctx, order = _setup(tmp_path)
    (tmp_path / "w.mot").write_bytes(b"S0030000FC")
    ctx.bench.scif["flash_writer"] = tmp_path / "w.mot"
    for fn in ("load_writer", "em_w", "em_secsd", "em_dcid"):
        monkeypatch.setattr(sw, fn, lambda *a, **k: None)
    monkeypatch.setattr(steps.Ctx, "artefact_bytes", lambda self, k: b"x")
    steps.Bootstrap().run(ctx)
    assert order[:4] == HALTED
    assert ctx.rom_console_on_count == ctx.bench.power.on_count      # the Flash Writer is live now


def test_dsw1_emmc_insert_sd_halts_before_it_asks_the_operator_to_power_off(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.bench.power.on_hook = lambda: ctx.bench.console.feed("Hit any key to stop autoboot: 3\r\n")
    steps.OpDsw1EmmcInsertSd().run(ctx)
    assert order[:3] == ["true", "ident", "poweroff"]
    assert order[3].startswith("confirm: The unit has been halted (poweroff, halt line seen). Power OFF, insert the provisioning microSD (DSW1 may stay on xSPI)")
    assert order.index("psu-off") > 3                                # the cold cycle comes after the prompt


def test_dsw1_xspi_remove_sd_halts_before_it_asks_the_operator_to_pull_the_card(tmp_path):
    ctx, order = _setup(tmp_path)
    res = steps.OpDsw1XspiRemoveSd().run(ctx)
    assert res.status == "done"
    assert order[:3] == ["true", "ident", "poweroff"]
    assert order[3] == ("confirm: The unit has been halted (poweroff, halt line seen). "
                        "Power OFF, set DSW1 to xSPI boot, REMOVE the microSD. Leave it OFF until the tool asks.")
    assert "psu-off" not in order                                    # the operator cuts power, not the tool


def test_the_operator_is_warned_when_the_unit_could_not_be_halted(tmp_path):
    ctx, order = _setup(tmp_path, halts=False)
    steps.OpDsw1XspiRemoveSd().run(ctx)
    assert "WARNING: the unit was NOT cleanly halted (fallback)" in order[-1]
    ctx2, order2 = _setup(tmp_path / "b", ssh=False)
    steps.OpDsw1XspiRemoveSd().run(ctx2)
    assert "WARNING: the unit was NOT cleanly halted (blind)" in order2[-1]


def test_the_xmodem_path_halts_before_its_cold_to_prompt(tmp_path, monkeypatch):
    ctx, order = _setup(tmp_path, script=[(r"^boot\r$", "login: ")])
    ctx.transfer = "xmodem"
    ctx.bench.raw = {"uboot": {"load_addr": 0x1000}}
    monkeypatch.setattr(steps.Ctx, "artefact", lambda self, role: tmp_path / "x.wic.gz")
    monkeypatch.setattr(steps.uboot, "cold_to_prompt", lambda *a, **k: order.append("cold_to_prompt") or "")
    monkeypatch.setattr(steps.uboot, "loadx_gzwrite", lambda *a, **k: None)
    logins = []
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: logins.append(1))
    _boot_patches(monkeypatch, order)
    monkeypatch.setattr(lt, "console_login", lambda *a, **k: logins.append(1))
    try:
        steps.BootSdLinux().run(ctx)
    except Exception:       # everything after the boot is out of scope here
        pass
    assert [o for o in order if o in ("poweroff", "cold_to_prompt")] == ["poweroff", "cold_to_prompt"]
    assert logins and ctx.console_login_on_count == ctx.bench.power.on_count   # the xmodem login is remembered


# --- the outcome record --------------------------------------------------------------------------

def test_a_step_that_cuts_power_records_how_in_its_evidence_and_log(tmp_path):
    ctx, order = _setup(tmp_path)
    ctx.bench.power.on_hook = lambda: ctx.bench.console.feed("=> ")
    res = steps.run_one(steps.Detect(), ctx)
    assert res.evidence["power_cut"] == "clean: ssh poweroff, halt line seen; sync rc=0"
    assert "power cut clean: ssh poweroff" in ctx.step_logs["detect"]


def test_a_step_without_a_cut_has_no_power_cut_key(tmp_path):
    ctx, _ = _setup(tmp_path)
    ctx.bench.console.feed("login: ")
    assert "power_cut" not in steps.run_one(steps.Census(), ctx).evidence


# --- census: supply-current screen ----------------------------------------------------------

class Metered(FakePower):
    def __init__(self, amps):
        super().__init__()
        self.amps, self.queries = amps, 0

    def current(self):
        self.queries += 1
        if isinstance(self.amps, Exception):
            raise self.amps
        return self.amps


def _census(tmp_path, monkeypatch, power):
    ctx = _ctx(tmp_path, bench=_bench(), execute=True)
    ctx.bench.power = power
    ctx.linux = Board()
    monkeypatch.setattr(steps.lt, "census", lambda *a, **k: ({"cpu_khz": "1800000"}, []))
    return ctx, steps.Census().run(ctx)


def test_census_records_one_current_reading_and_what_is_known_about_its_state(tmp_path, monkeypatch):
    p = Metered(0.172)
    ctx, res = _census(tmp_path, monkeypatch, p)
    assert res.evidence["psu_current_a"] == "0.172" and res.evidence["psu_current_state"] == "at-census"
    assert p.queries == 1 and "WARNING" not in res.detail


def test_census_warns_above_0_40_a_but_does_not_fail(tmp_path, monkeypatch):
    ctx, res = _census(tmp_path, monkeypatch, Metered(0.451))
    assert res.status == "done" and "WARNING: supply current 0.451 A > 0.40 A" in res.detail
    assert res.evidence["psu_current_a"] == "0.451"


def test_census_does_not_warn_at_exactly_the_threshold(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, Metered(0.40))
    assert "WARNING" not in res.detail


def test_census_without_scpi_power_has_no_current_keys(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, FakePower())
    assert "psu_current_a" not in res.evidence and "psu_current_state" not in res.evidence


def test_census_records_a_failed_current_query_as_unread(tmp_path, monkeypatch):
    _, res = _census(tmp_path, monkeypatch, Metered(BenchError("SCPI: still failing")))
    assert res.status == "done" and res.evidence["psu_current_a"].startswith("unread (")
    assert "psu_current_state" not in res.evidence


@pytest.mark.parametrize("bad", [float("nan"), float("inf")])
def test_census_treats_a_non_finite_reading_as_unread(tmp_path, monkeypatch, bad):
    _, res = _census(tmp_path, monkeypatch, Metered(bad))
    assert res.evidence["psu_current_a"].startswith("unread (") and "psu_current_state" not in res.evidence
    assert "WARNING" not in res.detail


def test_census_queries_only_the_configured_channel(tmp_path, monkeypatch):
    from provision.bench import ScpiPower
    sent = []

    class Sock:
        def sendall(self, data):
            sent.append(data.decode().strip())

        def recv(self, n):
            return b"0.200" + bytes([10])

        def close(self):
            pass
    p = ScpiPower("h", 1, 1, connect=lambda addr, timeout: Sock())
    _, res = _census(tmp_path, monkeypatch, p)
    assert res.evidence["psu_current_a"] == "0.200"
    assert sent == ["MEAS:CURR? CH1"]


def test_pmic_verify_names_a_register_it_could_not_read_instead_of_passing(tmp_path):
    ctx = _ctx(tmp_path, bench=_bench(), execute=True,
               expected_registers={"devices": {"act88760": {"addr": 0x25, "bus": "pmic", "registers": [
                   {"reg": 0x10, "expect": 0x08}]}}})
    ctx.linux = Board()
    ctx.linux.regs.clear()                  # every i2cget fails
    with pytest.raises(steps.Refused, match=r"act88760 0x25 reg 0x10 unread \(.*after 3 attempts"):
        steps.PmicVerify().run(ctx)
    assert ctx.linux.commands.count("i2cget -y -f 8 0x25 0x10") == 3


# --- review round 3 ----------------------------------------------------------------------------

def test_a_second_call_after_a_fallback_keeps_the_fallback_not_a_not_needed(tmp_path):
    ctx, order = _setup(tmp_path, halts=False)
    assert steps.clean_shutdown(ctx) == "fallback"
    n = len(order)
    assert steps.clean_shutdown(ctx) == "fallback"                  # not "not-needed: nothing to halt"
    assert len(order) == n                                          # and nothing is typed again
    assert ctx.power_cuts[-1] == "fallback: earlier cut on this power-on was fallback"
    assert steps.halt_note("fallback").startswith("WARNING: the unit was NOT cleanly halted")


def test_a_second_call_after_a_clean_halt_is_still_not_needed(tmp_path):
    ctx, _ = _setup(tmp_path)
    steps.clean_shutdown(ctx)
    assert steps.clean_shutdown(ctx) == "not-needed"


def test_a_silent_console_keeps_the_halt_across_the_operator_prompt(tmp_path):
    """The tool saw the halt line and a halted unit prints nothing: the prompt must not turn the
    next cut into "blind"."""
    ctx, order = _setup(tmp_path)
    steps.OpDsw1XspiRemoveSd().run(ctx)
    assert ctx.halted_on_count == ctx.bench.power.on_count
    assert "Leave it OFF until the tool asks." in order[-1]
    n = len(order)
    assert steps.clean_shutdown(ctx) == "not-needed"
    assert order[n:] == []
    ctx2, order2 = _setup(tmp_path / "e")
    ctx2.bench.power.on_hook = lambda: ctx2.bench.console.feed("Hit any key to stop autoboot: 3\r\n")
    steps.OpDsw1EmmcInsertSd().run(ctx2)
    assert "Leave it OFF until the tool asks." in order2[3]
    assert ctx2.halted_on_count != ctx2.bench.power.on_count      # the power-on after the halt


def test_console_output_after_the_halt_invalidates_it_at_the_operator_prompt(tmp_path):
    """The operator powered the unit back on before Enter: boot text on the console."""
    ctx, order = _setup(tmp_path)
    ctx.bench.operator.confirm = lambda msg: ctx.bench.console.feed("U-Boot 2025.01 (powered on)\r\n")
    steps.OpDsw1XspiRemoveSd().run(ctx)
    assert ctx.halted_on_count is None


def test_a_clean_halt_with_a_failing_first_sync_is_its_own_outcome_on_the_console_path(tmp_path):
    ctx, order = _setup(tmp_path, ssh=False, script=[(POWEROFF_LINE, "ALPSabc12345:1\r\n" + HALT)])
    ctx.console_login_on_count = ctx.bench.power.on_count
    assert steps.clean_shutdown(ctx) == "clean (sync rc=1)"
    assert ctx.power_cuts == ["clean (sync rc=1): console poweroff, halt line seen; sync rc=1"]
    assert "final sync reported an error (sync rc=1)" in steps.halt_note("clean (sync rc=1)")


def test_a_clean_halt_with_a_failing_first_sync_is_its_own_outcome_on_the_ssh_path(tmp_path):
    ctx, order = _setup(tmp_path, first_rc=5)
    assert steps.clean_shutdown(ctx) == "clean (sync rc=5)"
    ctx2, _ = _setup(tmp_path / "u")
    ctx2.linux.first_rc = "?"                                       # no ALPSYNC marker at all
    ctx2.linux.run = lambda cmd, **k: lt.CmdResult(0, "", "")
    assert steps.clean_shutdown(ctx2) != "clean"


def test_the_prompt_says_when_the_halt_came_with_a_sync_error(tmp_path):
    ctx, order = _setup(tmp_path, first_rc=1)
    steps.OpDsw1XspiRemoveSd().run(ctx)
    assert "halted (poweroff, halt line seen), but the final sync reported an error (sync rc=1)" in order[-1]


def test_a_sync_rc_split_across_reads_is_not_truncated(tmp_path):
    """`127` must not be read as `1` because a read boundary fell between the digits."""
    order: list[str] = []
    console = LoggedConsole([(POWEROFF_LINE, "ALPSabc12345:127\r\n" + HALT)], order)
    console.chunk = 1
    b = _bench(console=console)
    b.power = OrderedPower(order)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    ctx.console_login_on_count = b.power.on_count
    out = steps.clean_shutdown(ctx)
    assert out == "unreadable (sync rc=127; sync not found)"
    assert ctx.halted_outcome == out and "unreadable" in ctx.power_cuts[-1]


def test_ext4_errors_on_the_console_are_reported_verbatim_and_mark_the_root_unreadable(tmp_path):
    err = "EXT4-fs error (device mmcblk1p2): ext4_find_entry:1455: reading directory lblock 0\r\n"
    ctx, _ = _setup(tmp_path, ssh=False, script=[(POWEROFF_LINE, "ALPSabc12345:0\r\n" + err + HALT)])
    ctx.console_login_on_count = ctx.bench.power.on_count
    out = steps.clean_shutdown(ctx)
    assert out.startswith("unreadable (") and "EXT4-fs error (device mmcblk1p2)" in out
    assert "NOT cleanly halted" in steps.halt_note(out) and "EXT4-fs error" in steps.halt_note(out)


def test_a_log_line_containing_the_uboot_prompt_text_is_not_a_uboot_prompt(tmp_path, monkeypatch):
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    monkeypatch.setattr(lt, "console_login", lambda c, u, timeout=120.0: None)
    shell = "^C\r\n[  812.1] foo: state a => b\r\nroot@board:~# "
    ctx, order = _setup(tmp_path, ssh=False, script=[(r"^\x03$", shell)] + _poweroff_over_console())
    assert steps.clean_shutdown(ctx) == "clean"                     # shut down through the shell, not skipped
    assert order[1].startswith("console:sync; echo")


def test_the_probe_runs_the_real_console_login_and_only_enters_after_ruling_out_uboot(tmp_path, monkeypatch):
    """Not a stub: lt.console_login against a fake Linux console. Its first Enter comes after the
    Ctrl-C was answered with something that is not a U-Boot prompt."""
    from .test_provision_console_target import _Tty
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    monkeypatch.setattr(lt, "CONSOLE_SETTLE_S", 0.01)
    tty = _Tty()
    b = _bench(console=tty)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    steps.clean_shutdown(ctx)
    assert tty.writes[0] == b"\x03" and tty.writes[1] == b"\r"       # Ctrl-C first, the login's Enter after
    assert any(w.startswith(b"export TERM=dumb") for w in tty.writes)
    assert ctx.power_cuts[-1].startswith("fallback: console:")      # the fake never halts; the path was the shell


def test_the_probe_sends_nothing_but_ctrl_c_when_the_console_shows_a_uboot_prompt(tmp_path, monkeypatch):
    from .test_provision_console_target import _Tty
    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    tty = _Tty()
    tty._write_raw = lambda data: (tty.writes.append(data), setattr(tty, "_out", tty._out + b"\r\n=> "))[0]
    ctx = _ctx(tmp_path, bench=_bench(console=tty), execute=True)
    assert steps.clean_shutdown(ctx) == "not-needed"
    assert tty.writes == [b"\x03"]
