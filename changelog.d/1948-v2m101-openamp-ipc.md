### Changed — E1M-V2M101's CM33 board carries the OpenAMP/MHU-B carve-out (#1948)

`topology.m33_sm.openamp_ipc` was set only on E1M-V2N101, which left the
generated `alp_e1m_v2m101_m33_sm` devicetree without the OpenAMP
reserved-memory block, the `mbox1` MHU-B node and the CAN-FD analysis,
as if the V2M101 hardware differed. It doesn't: both SoMs carry the same
RZ/V2N die, the same MHU-B and the same CM33-NS DDR alias. E1M-V2M101 now
sets the flag and its board `.dts` is regenerated to match.

The A55 half of this link (the `4f70xxxx` UIO and reserved-memory nodes)
is not in any shipped Linux devicetree for either SoM. Board #1
(E1M-V2M103, 2026-09-28) shows `48000000-13fffffff : System RAM` with no
hole at the A55 view `0x4f700000`, and no `/sys/class/uio`. So OpenAMP
between the cores still needs that overlay on both families. BENCH:
the V2M101 CM33 image with the carve-out has not been booted yet.
