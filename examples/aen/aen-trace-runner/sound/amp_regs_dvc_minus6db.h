/* sound/amp_regs_dvc_minus6db.h -- bench A/B register override for both
 * TAS2563 (-DTR_SND_AMP_REGS_FILE=<this file>): the amp's digital volume
 * DVC_CFG1..4 (book 0, page 2, regs 0x0C..0x0F, Q2.30 big-endian; POR
 * 0x40000000 = 0 dB) set to 0x20000000 = -6.02 dB. Map from the Linux
 * tas2562 codec driver, which also binds "ti,tas2563"
 * (sound/soc/codecs/tas2562.h: TAS2562_DVC_CFG1..4 = TAS2562_REG(2, 0x0c..0x0f);
 * tas2562.c reg_defaults 0x40/0x40/0x00/0x00 and its dB table
 * round(10^(dB/20) * 2^30)). The SLASET3D page for these registers is not
 * on the build host -- verify against it.
 *
 * Why: moves 6 dB of headroom INSIDE the amp (after its RX path), where
 * TR_SND_VOLUME moves it before I2S. If the harshness goes away with this
 * and not with a synth change, the amp's DSP/output stage was overdriven. */
#define TR_SND_AMP_REGS \
	{ \
		{ 0, 2, 0x0C, 0x20 }, \
		{ 0, 2, 0x0D, 0x00 }, \
		{ 0, 2, 0x0E, 0x00 }, \
		{ 0, 2, 0x0F, 0x00 }, \
	}
