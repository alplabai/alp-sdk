### Fixed — DEEPX DX-M1 runtime path bench-verified end to end after firmware provisioning, service-mode finding recorded (Refs #482)

Follow-up to `changelog.d/482.md`: that slice left DX-M1 firmware
missing (`dxrt-cli -s` reported `Fail to initialize device 0 ...
Identify`). After a DEEPX-supplied DX-M1 firmware update on
E1M-V2M103, the DX-M1 now flash-boots firmware 2.4.0 and enumerates
as PCIe `1ff4:0000` Gen3 x2; `dx_dma` + `dxrt_driver` autoload at
boot, `/dev/dxrt0` comes up `0660 root:video` via this layer's
tightened udev rule, and `dxrt-cli -s` reports rc `0` against the
pinned stack (`DXRT v3.2.0`, RT driver `v1.8.0`, PCIe driver `v1.6.0`,
FW `v2.4.0`, LPDDR5x `6000` Mbps / `3.92` GiB, Board `M.2 Rev 0.2`,
NPU0-2 `1000` MHz).

**`dxrtd` service-mode finding:** investigated whether `meta-deepx-m1`
ships or enables a `dxrtd` systemd unit. It does not, at this pin:
the pinned `dx-rt_3.2.0.bb` recipe builds with `USE_SERVICE=OFF`
(upstream's own default) and never fetches or installs a service
unit; a sibling `dx-rt_3.2.0-1.bb` recipe variant exists with
`USE_SERVICE=ON`, but its systemd/SysVinit wiring is commented out in
`meta-deepx-m1` itself, so even that variant ships no working
service. `run_model` / `dxrt-cli` (the bench-verified path above) do
not need service mode -- it exists only for multi-process device
sharing -- so no bbappend enabling one is added here; a broken unit
against a `USE_SERVICE=OFF` build would be worse than none.
`docs/soms/v2n-m1.md`'s new "DEEPX DX-M1 bring-up" section documents
the finding and the NAND UART provisioning procedure.

`metadata/chips/deepx_dxm1.yaml`'s `verification.hil_silicon` stays
`untested` (unchanged): it tracks this chip's own host-side C
bring-up sequencer, which V2N-M1 does not use (U-Boot re-implements
the sequence with raw register pokes) -- the bench evidence above
verifies the separate Yocto runtime path, not this driver, and the
manifest comment now says so explicitly to prevent conflating the
two. `vendors/deepx-dxm1/README.md` gains a matching status note,
distinguishing the now-verified runtime path from the still
BENCH-UNVERIFIED SDK inference backend (`src/yocto/inference_deepx.cpp`).

Real `.dxnn` model inference through the SDK's own backend remains
open per #482's acceptance criteria.
