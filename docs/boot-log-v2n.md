# V2N/V2M boot log: what each warning means

These lines were collected from several boots of E1M-V2M103 bench units in the
E1M-X EVK (U-Boot 2024.07-alp, Linux 6.1.141-cip43) on the image before the
changes below, in different provisioning states: with and without a DX-M1
firmware, with and without a provisioned EEPROM, SD and eMMC root. They are
not the output of one boot, and some rows exclude each other (the PCIe `-110`
row cannot occur on a boot that shows the `dx_dma` rows). The captures cover
the first ~20 s of boot, up to the login prompt. Each row says when it applies.

## Changed in this branch; build-checked; not yet run on a board

Nothing here has run on a board. The kernel, U-Boot, `dx-rt` and the
`alp-image-base` rootfs image for `e1m-v2m103-a55` were built in a container.

| Line | Cause | Change |
|---|---|---|
| `Failed to load 'boot/r9a09g056n44-dev.dtb'` (U-Boot) | the vendor `sd2load`/`emmcload` env names the stock EVK dtb | the image links that name to the board dtb (#2637). The image was built in a container and the ext4 holds `/boot/r9a09g056n44-dev.dtb -> e1m-v2m101-x-evk.dtb`; whether U-Boot's `ext4load` follows it was not run |
| `irq 14: nobody cared` then `Disabling IRQ #14` | the shared ICU error line stormed before its mask was written | patch 0011 masks before requesting the line (#2632). After the fix the boot log still carries `rzv2h_icu 10400000.interrupt-controller: error group 0 status 0x02000000 pending at probe, masked`: that warning is the replacement, see the benign table |
| `genirq: Setting trigger mode 3 for irq 154 failed (rzg2l_gpio_irq_set_type+0x0/0x20)` (irq 143/144 on other boots) | SD card-detect asks for both edges; the ICU TINT has no both-edge mode | `broken-cd` on `&sdhi1` (#2636): the MMC core polls the card-detect GPIO about once a second and requests no interrupt, so card insertion/removal is still detected within about a second, as before. Built into the dtb; not run on a board |
| `brcmfmac mmc2:0001:1: Direct firmware load for cypress/cyfmac55500-sdio.alp,e1m-v2m101-x-evk.trxse failed with error -2`, and the two `brcmfmac: brcmf_fw_request_firmware: no board-specific nvram available (ret=-2), device will use cypress/cyfmac55500-sdio.txt` / `….clm_blob` lines | the driver tries board-named firmware first | board-named symlinks for `.trxse`, `.txt`, `.clm_blob` (#2638); whether the two nvram/clm lines disappear was not confirmed (the backports driver source was not read) |
| `PHY [stmmac-N:02] driver [RTL8211F-VD Gigabit Ethernet]` | the DT forced the VD id; the silicon reports `0x001c.c916` | the DT forces c916 (#2631); see errata E2 |

## Expected or benign

| Line | Applies when | Why |
|---|---|---|
| `Loading Environment from MMC... *** Warning - bad CRC, using default environment` and `## Resetting to default environment` (U-Boot) | every boot | no environment is stored today; `bootcmd` runs `env default -a` every boot. Revisit with the OTA design (`docs/ota.md`) |
| `rzv2h_icu 10400000.interrupt-controller: error group 0 status 0x02000000 pending at probe, masked` | every boot with the Cortex-M33 running | a non-GPT ICU error source is asserted at probe and is masked on purpose by patch 0011 |
| `efi: UEFI not found.`, `EFI services will not be available.`, `DMI not present or invalid.` | every boot | the board boots from U-Boot with a DT: no UEFI or SMBIOS |
| `psci: MIGRATE_INFO_TYPE not supported.`, `SMCCC: SOC_ID: ARCH_SOC_ID not implemented, skipping ....` | every boot | optional firmware calls TF-A does not implement |
| `Console: colour dummy device 80x25` | every boot | no VGA console; the serial console is used |
| `iommu: DMA domain TLB invalidation policy: strict mode` | every boot | the default policy |
| `kvm [1]: HYP mode not available` | every boot | the kernel is entered at EL1 |
| `CPU features: detected: ARM errata 1165522, 1319367, or 1530923`, `CPU features: detected: Qualcomm erratum 1009, or ARM erratum 1286807, 2441009` | every boot | Cortex-A55 workarounds being applied |
| `cacheinfo: Unable to detect cache hierarchy for CPU N` | every boot | the SoC dtsi (also mainline) describes only the L3 |
| `thermal emergency: set temperature to 110 celsius`, `thermal emergency: shutdown target cpus 1-3`, `thermal emergency: freq scaled target cpus 0` | every boot, about 2 s | printed once at thermal-zone registration, before any load; read as the configured emergency policy, not an event. Not traced to source: not yet explained beyond that |
| `pci_bus 0000:01: 2-byte config write to 0000:01:00.0 offset 0x4 may corrupt adjacent RW1C bits` | boots with the DX-M1 PCIe link up | PCI core warning about a 16-bit config write by the DX-M1 driver stack; not yet explained from source |
| `dx_dma_pcie 0000:01:00.0: Unbalanced pm_runtime_enable!` | boots with the DX-M1 link up | the DEEPX `dx_dma` driver calls `pm_runtime_enable()` (`dw-edma-core.c`) on a device whose runtime PM is already enabled |
| `debugfs: Directory '0000:01:00.0' with parent 'dmaengine' already present!` | boots with the DX-M1 link up | `dx_dma` registers a read and a write dmaengine device on one `struct device`; only debugfs is affected |
| `dx_dma: loading out-of-tree module taints kernel.` | boots with the DX-M1 link up | the DEEPX driver is out of tree |
| `dxrt_recovery_device` | boots with the DX-M1 up, right after `Started DX-RT Service` | `pr_info` in the DEEPX driver: the `dxrtd` service sends its startup recovery command (`dxrt_service.cpp`); not an error |
| `get() with no identifier` | every boot, during the Mali GPU probe | `pr_err` in the regulator core, a `regulator_get()` with a NULL supply name; the GPU still probes (`Probed as mali0`). Cause not traced to source: not yet explained |
| `mali 14850000.gpu: * MALI kbase_mmap_min_addr compiled to CONFIG_DEFAULT_MMAP_MIN_ADDR, no runtime update possible! *` | every boot | the Mali kbase driver notice about the compiled-in mmap limit |
| `renesas_usbhs 15820000.usb: no transceiver found` | every boot | the USB2 gadget/host controller has no OTG transceiver node |
| `asoc-audio-graph-card2 sound: Audio Graph Card2 is still under Experimental stage` | every boot | the audio-graph-card2 driver's own notice |
| `asoc-audio-graph-card2 sound: ASoC: driver name too long 'e1m-x-evk-tas2563'` | every boot | truncated to 15 characters; renaming would change the ALSA card id |
| `ehci-platform … overcurrent ignored` | every boot | deliberate, see errata E3 |
| `usb usb2: We don't know the algorithms for LPM for this host, disabling LPM.` | every boot | xHCI without LPM parameters; USB 3 works without LPM |
| `hci_uart_bcm … supply vbat/vddio not found, using dummy regulator` | every boot | the module rails are always on; no public rail data to model them |
| `clk: Disabling unused clocks` | every boot | normal late-boot clock cleanup |
| `systemd[1]: Watchdog running with a hardware timeout of 30s.` | every boot | the CA55 watchdog policy |
| `systemd-journald: Collecting audit messages is disabled.`; systemd `Journal Audit Socket`, `Kernel Trace File System`, `Create List of Static Device Nodes`, `File System Check on Root Device` `was skipped because of an unmet condition check` | every boot | no kernel audit (`no-kernel-audit.cfg`), no tracefs, no `modules.devname`, a read-write root |
| `systemd[1]: System time before build time, advancing clock.` and `rtc-rv3028 8-0052: hctosys: unable to read the hardware clock` | every cold boot on the E1M-X EVK | the RV-3028 lost power (its power-on-reset flag is set). It keeps time across a power cycle only if the carrier supplies VBACKUP on pad `P10` (see `docs/soms/v2n.md`, Real-time clock), so on the EVK this prints on every cold boot and the time is set again over NTP; a warm reboot keeps it. Whether the backup switchover mode also has to be enabled was not checked |
| `sd 0:0:0:0: [sda] …` | a USB mass-storage stick is plugged in | the stick on the xHCI port (a SanDisk Cruzer Blade on the bench units) |

## Depends on the unit's state

| Line | Applies when | Why |
|---|---|---|
| `rzg3s-pcie-host: probe of 13400000.pcie failed with error -110` | the DX-M1 has no firmware | the PCIe link does not train until the firmware is provisioned; it cannot appear together with the `dx_dma` rows |
| `ALP: WARNING: no validated SoM serial …` | the SoM EEPROM is not provisioned | the MAC falls back to the eMMC CID |
| `gpio-gd32-bridge … bridge not answering (-6)` | the GD32 did not answer at probe | no firmware yet, or (early units, FIP older than U-Boot patch 0011) the GD32 is still held in reset, or the probe ran before the GD32 was up (kernel patch 0005); the gpiochip is still registered. A provisioned unit prints `GD32 bridge protocol N` instead |
| `Card did not respond to voltage select! : -110` (U-Boot, twice) | the SD slot is empty | the boot script probes `mmc 1`; U-Boot has no quiet probe |
| `mmc1: Card stuck being busy! __mmc_poll_for_busy` | an image with the slot at SDR104 | the DT now caps the slot at SDR50 (#2357), so the shipped image does not print it |
| `rz-sci 12801c00.serial: Failed to create device link (0x180) with 8-0070` | every boot | fw_devlink: the Bluetooth child of `&sci4` takes its `shutdown-gpios` from the GD32 bridge after `&sci4` is bound; the link is sync-state-only, so Bluetooth ordering is handled by the gpio probe deferral |
