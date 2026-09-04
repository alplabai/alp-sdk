/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dependency-free guard shared by the Yocto (yocto_drv.c) and Zephyr
 * (zephyr_drv.c) audio-out backends' `out_set_volume`.  Both backends
 * apply the software volume scale on S16_LE only (#632 / zephyr's
 * `be->volume_q8` path); a non-unity request on any other open format
 * can never actually be applied, so it must be refused HERE, at the
 * entry point the caller can see, rather than silently dropped deeper
 * in the write path (issue #1648 tier 1).
 *
 * 255 (ALP_AUDIO_CONFIG's linear full-scale sentinel -- see
 * alp_audio_out_set_volume's doc comment in <alp/audio.h>) is always a
 * no-op scale-wise, so it must succeed regardless of format; refusing
 * it would break callers who harmlessly set 255 on a format this SDK
 * can't yet scale.
 *
 * Header-only + no Zephyr/ALSA includes (mirrors
 * src/backends/adc/adc_oversampling.h's shape) so
 * tests/unit/audio_volume_guard can exercise it hermetically on
 * native_sim with no I2S / ALSA device involved.
 */

#ifndef ALP_AUDIO_VOLUME_GUARD_H
#define ALP_AUDIO_VOLUME_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/audio.h>

/**
 * @brief True when @p vol can be applied to a stream opened with @p format.
 *
 * @param[in] format  The format the handle was opened with.
 * @param[in] vol     Linear 0..255 volume, as passed to out_set_volume.
 */
static inline bool alp_audio_volume_settable(alp_audio_format_t format, uint8_t vol)
{
	return (vol == 255u) || (format == ALP_AUDIO_FMT_S16_LE);
}

#endif /* ALP_AUDIO_VOLUME_GUARD_H */
