### Fixed — the Yocto/ALSA I²S backend threw away the negotiated sample rate, so an unsupported rate opened successfully and streamed at the wrong speed (#1648 tier 1)

`src/backends/i2s/yocto_drv.c`'s `_configure_pcm()` kept only the return code from
`snd_pcm_hw_params_set_rate_near()`, discarding the rate ALSA actually negotiated back through
its `&rate` argument. Asking for a rate the DAI doesn't support used to let `alp_i2s_open()`
succeed while every subsequent `alp_i2s_write()`/`alp_i2s_read()` streamed at whatever rate ALSA
picked instead — silently wrong, and in direct contradiction of `alp_i2s_open()`'s own doc
comment, which promises "an unsupported sample rate ... returns NULL here, not later."
`_configure_pcm()` now compares the negotiated rate against the request and fails open with
`ALP_ERR_NOSUPPORT` on a mismatch, logging both numbers.

**Scope: I²S class only.** `src/backends/audio/yocto_drv.c`'s `configure_pcm()` has the identical
`set_rate_near`-discards-`&rate` shape and carries the same silent-wrong-rate defect today — this
change does **not** touch it. That fix is scoped to a separate, still-open, bench-held PR (#1790)
that has not merged to `dev`. Until it lands, `alp_audio_out_open()`/`alp_audio_in_open()` on the
Yocto/ALSA path can still silently accept an unsupported rate and stream wrong.

The negotiated *period* is deliberately **not** enforced the same way, for the same reason the
audio backend's period is a hint rather than a contract there too: `block_frames` is a DMA-block
sizing hint, `snd_pcm_writei()` accepts any frame count per call regardless of the device's
period (`snd_pcm_readi()` likewise — it reports whatever count it actually read through
`bytes_out`, not a fixed period-sized chunk), and a `default` ALSA device commonly negotiates its
own fixed period (e.g. a dmix plugin) regardless of what `block_frames` asks for — refusing on a
period mismatch would fail `ALP_I2S_CONFIG_DEFAULT`'s 256-frame request against exactly that kind
of device. `_configure_pcm()` adopts whatever ALSA negotiates for buffer sizing and only logs
when the period differs from the request.

**Known gap, not touched here.** `alp_i2s_write()`'s doc comment (`include/alp/i2s.h`) also
promises that a `bytes` argument larger than the block size negotiated at open is "refused with
`ALP_ERR_OUT_OF_RANGE` rather than truncated," and `src/backends/i2s/zephyr_drv.c`'s `z_write()`
enforces exactly that. `y_write()` does not: it loops `snd_pcm_writei()` across as many ALSA
periods as `bytes` needs, so an oversize write still completes correctly instead of being
refused — a real, if benign, deviation from the documented contract that this change does not
fix.

No new capability, no ABI change: an honest `ALP_ERR_NOSUPPORT` refusal in place of a silent wrong
answer, per ADR-0002. `src/backends/i2s/zephyr_drv.c` was checked as the sibling backend and needs
no equivalent fix — Zephyr's `i2s_configure()` takes `frame_clk_freq` as an exact request and
fails outright rather than silently substituting a nearby rate, so it never had this defect.

**Bench-unverified.** This changes accepted behaviour on real hardware: a config that previously
opened successfully and streamed at a substituted rate now refuses at `alp_i2s_open()` with
`ALP_ERR_NOSUPPORT` when the negotiated rate doesn't match the request — for example,
`ALP_I2S_CONFIG_DEFAULT`'s 48000 Hz against a minimal Yocto image whose `/etc/asound.conf` aliases
`default` straight to a 44.1 kHz-only DAI with no resampling plugin. That refusal is the intended,
documented behaviour, but it is a behaviour change that has not been exercised on a Yocto target
with a real I²S DAI. Like the rest of `src/backends/i2s/yocto_drv.c`, this path is real-ALSA-device
code with no existing hermetic test harness (see the file's own `STATUS: ... BENCH-UNVERIFIED`
note) — bench time on real hardware is needed to observe the refusal firing correctly, matching
the `needs-silicon` label already on #1648. No bench evidence is claimed for this change.
