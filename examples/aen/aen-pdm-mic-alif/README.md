# aen-pdm-mic-alif

Capture PCM audio from the EVK's **PDM microphones** (4× MP34DT05) on the
E1M-AEN801 (Alif Ensemble **E8**, M55-HE), through the Ensemble **HP PDM** block
(`pdm@4902d000`) and the vendored `alif,alif-pdm` DMIC driver. Drives the standard
Zephyr **DMIC API** (`dmic_configure` / `dmic_trigger` / `dmic_read`) on
`DT_ALIAS(alp_pdm0)`.

## The block

The E8 has two PDM instances: the **HP `pdm@4902d000`** (main / EXPMST0 domain) and
the low-power `lppdm@43002000` (M55-HE local). The **E1M-AEN801 routes its mics to
the HP PDM** — per the authoritative SoM pinout
`metadata/e1m_modules/aen/from-alif.tsv`:

| Mic signal | SoC function | Pad | HP-PDM channel |
|---|---|---|---|
| `PDM_C0` | `PDM_C0_C` | P6_1 | clock 0 |
| `PDM_D0` | `PDM_D0_C` | P6_0 | data → ch 0/1 |
| `PDM_C1` | `PDM_C2_B` | P11_4 | clock 2 |
| `PDM_D1` | `PDM_D2_B` | P5_4 | data → ch 4/5 |

So two stereo data lines (D0→ch0/1, D2→ch4/5) = the 4 mics. Upstream Zephyr v4.4 +
`hal_alif` ship no Alif PDM driver, so it's vendored from the Apache-2.0 fork
(`drivers/audio/alif_pdm.c`) as an **ADR 0017 Tier-2** copy.

## Status

**The PDM peripheral is fully + correctly configured on E8 — every bit SWD-verified
— but it does not yet sample, because the 76.8 MHz audio clock SOURCE is not
enabled (it is SE-managed).** RESULT PARTIAL.

| Register (SWD) | Value | Meaning |
|---|---|---|
| `PDM_CONFIG` (`0x4902D000`) low byte | `0x33` | channels 0,1,4,5 **enabled** |
| `PDM_CONFIG` bit 16 (`PDM_CLK_MODE`) | `1` | mode `STANDARD_VOICE_512` (**not** sleep) |
| `EXPMST0_CTRL` (`0x4902F000`) bit 8 | `1` | HP-PDM clock gate **on** |
| `EXPMST0_CTRL` bits 30/31 (PCLK/IPCLK force) | `1` | EXPMST0 IP clock **forced on** |
| `PDM_FIFO_STATUS` (`0x4902D00C`) | `0` | FIFO empty → **not sampling** |

Getting the config this correct took finding several real bugs (the first cut had
*all* of them): wrong PDM **instance** (LPPDM → HP per the SoM TSV); wrong **pads**;
the data pads lacked `input-enable` (the upstream pad REN bit); the app never called
`pdm_mode()` / `pdm_channel_config()` (so the block sat in `MICROPHONE_SLEEP`); the
channel mask wasn't the raw PDM mask; and the EXPMST0 `IPCLK/PCLK_FORCE` bits (from
the fork clock driver) weren't set.

> **Root cause of no-capture (definitive, not inventable):** `FIFO=0` (rather than
> a FIFO full of constant/DC samples) means the PDM is **not clocking** at all. The
> 76.8 MHz PDM functional clock comes from **`CLKEN_HFOSCx2`**, which on the E8 is
> owned by the **Secure Enclave** and must be requested via the `se_services` library
> over the **MHU mailbox to the SE** (`SERVICES_clocks_enable_clock(...,
> CLKEN_HFOSCx2)`). alp-sdk does **not** integrate `se_services`/MHU yet (no caller
> anywhere), so the audio clock is off and the block never samples.
>
> **To finish (a scoped follow-up, a new subsystem — not a peripheral fix):**
> integrate `hal_alif/se_services` + the MHU-to-SE transport, get a services handle,
> and enable `CLKEN_HFOSCx2` before `dmic_trigger(START)`. Then this example should
> capture live PCM (RESULT PASS). Confirm the mic supply is on too. See
> [[project_pending_hw_configs]].
