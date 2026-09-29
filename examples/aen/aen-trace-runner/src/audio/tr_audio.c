/* src/audio/tr_audio.c -- see tr_audio.h.
 *
 * Voices: an oscillator (square/saw/triangle/sine/noise) with a linear pitch
 * slide, a one-pole filter (low- or high-pass, coefficient slide), a linear
 * ADSR in Q24, a start delay (so one event can schedule a whole phrase) and
 * a Q15 level. Voices 0..V_MUSIC-1 belong to the music sequencer, one per
 * instrument; the rest are a pool the SFX allocate from. When the pool is
 * full, V1/V2 steal round-robin; V3 steals the voice allocated longest ago,
 * which is never one of the event being started unless that event alone
 * needs more than the whole pool.
 *
 * Integer only. The one 64-bit multiply per sample (music gain) and the
 * 64-bit multiply per note-on (Hz -> phase increment) are multiplies, never
 * divisions; the per-note-on divisions are 32-bit.
 */
#include "tr_audio.h"

#include <string.h>

#include "../ipc/tr_mbox.h" /* TR_CRASH_KIND_WIRE */

enum { W_OFF, W_SQUARE, W_SAW, W_TRI, W_SINE, W_NOISE };
enum { F_NONE, F_LP, F_HP };
enum { S_ATTACK = 1, S_DECAY, S_RELEASE };

#define ENV_ONE (1 << 24)
#define V_MUSIC 6
#define V_SFX \
	V3(12, 16) /* V3: a wire crash alone takes 14 (3 impact + 6 debris + zap + 4 crackle) */
#define V_TOTAL (V_MUSIC + V_SFX)
#define MS(ms)  ((uint32_t)(ms) * TR_AUDIO_RATE / 1000u)
#define HZ(hz)  ((uint32_t)(hz) * 256u)             /* Q8 Hz */
#define CUT(pm) ((uint16_t)((pm) * 65535u / 1000u)) /* filter coefficient, per mille */

/* Music instruments = fixed voice slots. */
enum { M_BASS, M_ARP, M_LEAD, M_KICK, M_SNARE, M_HAT };

/* Music level (Q15) per TR_MUSIC_* and while ducked under a crash/sting. */
#define GAIN_PLAY    32768
#define GAIN_ATTRACT 18000
#define DUCK_PCT     35
#define GAIN_RAMP    (64000 / (int32_t)TR_AUDIO_RATE) /* per sample: a full swing in ~0.5 s */
#define MASTER_Q8    V3(384, MASTER_V3_Q8) /* output = mix * MASTER_Q8 / 256; V1/V2 x1.5 */
#define LIM_KNEE     24576
#define LIM_RANGE    (32767 - LIM_KNEE)
/* V2 level trims: crash peaked -4.6 dBFS and its debris squares were raw */
#if TR_AUDIO_V2
#define V2(v1, v2) (v2)
#else
#define V2(v1, v2) (v1)
#endif
/* V3 master: re-levelled against the 2026W36-0002 speaker response so the mic
 * hears the music at least as loud as V2 did (see the round-4 commit). */
#ifndef MASTER_V3_Q8
#define MASTER_V3_Q8 480 /* x1.875: predicted mic music +4 dB over V2 (tS3) */
#endif
#if TR_AUDIO_V3
#define V3(old, v3) (v3)
#else
#define V3(old, v3) (old)
#endif

/* 4th-order Butterworth high-pass at TR_AUDIO_HPF_HZ as two RBJ biquads
 * (Q 0.5412, 1.3066), each normalised by a0, Q28: b0 b1 b2 a1 a2. */
#if TR_AUDIO_RATE == 16000u
static const int32_t k_hpf[2][5] = { { 229131008, -458262016, 229131008, -454665656, 193422920 },
	                                 { 249556482, -499112964, 249556482, -495196013, 234594459 } };
#else
static const int32_t k_hpf[2][5] = { { 254367729, -508735458, 254367729, -508293902, 240741557 },
	                                 { 262293491, -524586983, 262293491, -524131669, 256606841 } };
#endif

static int32_t biquad(const int32_t *k, tr_biquad_t *st, int32_t x)
{
	/* y is kept with 12 fraction bits and the output truncated toward zero:
	 * a plain Q0 state sat in a -48 LSB dead band after the last voice
	 * (measured; Q8 still left -2 LSB at 48 kHz), so the idle output never
	 * returned to digital silence. Products stay below 2^59. */
	int64_t acc = ((int64_t)k[0] * x + (int64_t)k[1] * st->x1 + (int64_t)k[2] * st->x2) * 4096 -
	              (int64_t)k[3] * st->y1 - (int64_t)k[4] * st->y2;
	int32_t yq  = (int32_t)(acc >> 28);
	st->x2      = st->x1;
	st->x1      = x;
	st->y2      = st->y1;
	st->y1      = yq;
	return (yq + (yq < 0 ? 4095 : 0)) / 4096; /* toward zero */
}

int32_t tr_audio_hpf(tr_hpf_t *st, int32_t x)
{
	return biquad(k_hpf[1], &st->s[1], biquad(k_hpf[0], &st->s[0], x));
}

typedef struct {
	uint8_t  wave, filt, stage, pad;
	uint32_t phase, inc, duty;
	int32_t  slide; /* added to inc per sample while slide_n > 0 */
	uint32_t slide_n;
	int32_t  env, a_step, d_step, r_step, sus; /* Q24 */
	uint32_t hold;                             /* samples left before release */
	uint32_t delay;                            /* samples left before the note starts */
	int32_t  vol;                              /* Q15 */
	int32_t  lp;                               /* filter state */
	int32_t  cut;                              /* Q24 coefficient */
	int32_t  cut_slide;
	uint32_t cut_n;
	uint32_t noise;
	int32_t  noise_s;
	uint32_t seq, owner; /* V3: allocation order, and the tr_audio_event() that allocated it */
} voice_t;

/* One note, in musician's units; play() converts to per-sample steps. */
typedef struct {
	uint8_t  wave, filt, duty, sus; /* duty: square high part /256 (0 = 50%); sus: % */
	uint32_t hz0,
	    hz1; /* Q8 Hz; hz1 == 0: no slide. Noise: sample-and-hold rate, 0 = every sample */
	uint16_t a_ms, d_ms, hold_ms, r_ms;
	uint16_t cut0, cut1; /* coefficient fraction /65535; cut1 == 0: fixed */
	uint16_t delay_ms;
	int16_t  vol;
} note_t;

static struct {
	voice_t  v[V_TOTAL];
	uint32_t rng;
	uint32_t k_inc; /* phase increment per 1 Hz: 2^32 / TR_AUDIO_RATE */
	uint32_t clipped, limited;
	tr_hpf_t hpf;
	unsigned steal;
	uint32_t alloc_seq, ev_serial;
	/* music */
	uint8_t  mode;
	int32_t  gain, target;
	uint32_t duck;
	uint32_t step, step_pos;
	bool     playing;
} g;

static uint32_t rnd(void)
{
	uint32_t x = g.rng;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g.rng = x;
	return x;
}

/* Equal temperament, octave 0 (C0 = MIDI 12), Q8 Hz. */
static const uint32_t k_oct0_q8[12] = { 4186, 4435, 4699, 4978, 5274, 5588,
	                                    5920, 6272, 6645, 7040, 7459, 7902 };

static uint32_t midi_q8(unsigned n)
{
	return k_oct0_q8[n % 12u] << (n / 12u - 1u); /* n >= 12 */
}

static uint32_t inc_of(uint32_t hz_q8)
{
	return (uint32_t)(((uint64_t)hz_q8 * g.k_inc) >> 8);
}

#if TR_AUDIO_V2
/* polyBLEP residual (Q15) for a unit step at phase 0: t = phase, dt =
 * phase increment, both Q32. Non-zero only within one sample of the step,
 * so the 32-bit divisions run twice per period, not per sample. */
static int32_t blep(uint32_t t, uint32_t dt)
{
	uint32_t d = dt >> 16;

	if (d == 0u) {
		return 0;
	}
	if (t < dt) {
		int32_t x = (int32_t)((t >> 16) * 32768u / d);
		return 2 * x - ((x * x) >> 15) - 32768;
	}
	if (t > 0u - dt) {
		int32_t x = (int32_t)(((0u - t) >> 16) * 32768u / d);
		return ((x * x) >> 15) - 2 * x + 32768;
	}
	return 0;
}

/* Filter coefficients are written as fractions at 16 kHz; at 48 kHz the
 * same cutoff needs 1 - (1 - c)^(1/3). Integer cube root, Q16. */
static uint16_t cut_at_rate(uint16_t c16)
{
	if (TR_AUDIO_RATE == 16000u || c16 == 0u) {
		return c16;
	}
	uint64_t y  = (uint64_t)(65535u - c16) << 32; /* (1 - c) in Q48 */
	uint32_t lo = 0, hi = 65536;
	while (lo < hi) { /* largest x (Q16) with x^3 <= y */
		uint32_t mid = (lo + hi + 1u) / 2u;
		if ((uint64_t)mid * mid * mid <= y) {
			lo = mid;
		} else {
			hi = mid - 1u;
		}
	}
	return (uint16_t)(65535u - (lo > 65535u ? 65535u : lo));
}
#else
static uint16_t cut_at_rate(uint16_t c16)
{
	return c16;
}
#endif

static void play(voice_t *v, const note_t *n, bool retrigger)
{
	uint32_t hold = MS(n->hold_ms);

	if (!retrigger || v->wave == W_OFF) {
		memset(v, 0, sizeof(*v));
		v->noise = rnd() | 1u;
	}
	v->wave = n->wave;
	v->filt = n->filt;
	v->duty = (uint32_t)(n->duty ? n->duty : 128u) << 24;
	v->inc  = inc_of(n->hz0);
#if TR_AUDIO_V2
	if (n->wave == W_NOISE && n->hz0 == 0u && TR_AUDIO_RATE > 16000u) {
		v->inc = inc_of(HZ(16000)); /* keep the 16 kHz noise colour */
	}
#endif
	v->slide   = 0;
	v->slide_n = 0;
	if (n->hz1 != 0u && hold != 0u) {
		v->slide   = ((int32_t)inc_of(n->hz1) - (int32_t)v->inc) / (int32_t)hold;
		v->slide_n = hold;
	}
	uint16_t c0  = cut_at_rate(n->cut0);
	v->cut       = (int32_t)c0 << 8;
	v->cut_slide = 0;
	v->cut_n     = 0;
	if (n->cut1 != 0u && hold != 0u) {
		v->cut_slide = (((int32_t)cut_at_rate(n->cut1) - (int32_t)c0) << 8) / (int32_t)hold;
		v->cut_n     = hold;
	}
	v->a_step = n->a_ms ? ENV_ONE / (int32_t)MS(n->a_ms) : ENV_ONE;
	v->d_step = n->d_ms ? ENV_ONE / (int32_t)MS(n->d_ms) : ENV_ONE;
	v->r_step = n->r_ms ? ENV_ONE / (int32_t)MS(n->r_ms) : ENV_ONE;
	v->sus    = (int32_t)(ENV_ONE / 100) * n->sus;
	v->stage  = S_ATTACK;
	v->hold   = hold ? hold : 1u;
	v->delay  = MS(n->delay_ms);
	v->vol    = n->vol;
}

static int32_t voice_tick(voice_t *v)
{
	if (v->delay != 0u) {
		v->delay--;
		return 0;
	}

	uint32_t ph = v->phase;
	int32_t  s;

	switch (v->wave) {
	case W_SQUARE:
		s = ph < v->duty ? 32767 : -32767;
#if TR_AUDIO_V2
		s += blep(ph, v->inc) - blep(ph - v->duty, v->inc);
		s = s > 32767 ? 32767 : (s < -32767 ? -32767 : s);
#endif
		break;
	case W_SAW:
		s = (int32_t)(ph >> 16) - 32768;
#if TR_AUDIO_V2
		s -= blep(ph, v->inc);
		s = s > 32767 ? 32767 : (s < -32767 ? -32767 : s);
#endif
		break;
	case W_TRI: {
		int32_t p = (int32_t)(ph >> 15);
		s         = p < 65536 ? p - 32768 : 98303 - p;
		break;
	}
	case W_SINE: {
		/* parabolic sine: 4x(1-|x|) over one period, peak clamped */
		int32_t x  = (int32_t)(ph >> 16) - 32768;
		int32_t ax = x < 0 ? -x : x;
		s          = (x * (32768 - ax)) >> 13;
		s          = s > 32767 ? 32767 : (s < -32767 ? -32767 : s);
		break;
	}
	default: /* W_NOISE: new random sample on each phase wrap (every sample if inc == 0) */
		if (v->inc == 0u || ph + v->inc < ph) {
			uint32_t x = v->noise;
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			v->noise   = x;
			v->noise_s = (int32_t)(x >> 16) - 32768;
		}
		s = v->noise_s;
		break;
	}
	v->phase = ph + v->inc;
	if (v->slide_n != 0u) {
		v->inc += (uint32_t)v->slide;
		v->slide_n--;
	}

	if (v->filt != F_NONE) {
		int32_t c = v->cut >> 9; /* Q15, <= 32768 */
		v->lp += ((s - v->lp) * c) >> 15;
		s = v->filt == F_LP ? v->lp : s - v->lp;
		if (v->cut_n != 0u) {
			v->cut += v->cut_slide;
			v->cut_n--;
		}
	}

	switch (v->stage) {
	case S_ATTACK:
		v->env += v->a_step;
		if (v->env >= ENV_ONE) {
			v->env   = ENV_ONE;
			v->stage = S_DECAY;
		}
		break;
	case S_DECAY:
		if (v->env > v->sus) {
			v->env -= v->d_step;
			if (v->env < v->sus) {
				v->env = v->sus;
			}
		}
		break;
	default: /* S_RELEASE */
		v->env -= v->r_step;
		if (v->env <= 0) {
			v->env  = 0;
			v->wave = W_OFF;
			return 0;
		}
		break;
	}
	if (v->stage != S_RELEASE && --v->hold == 0u) {
		v->stage = S_RELEASE;
	}
	if (v->stage == S_DECAY && v->sus == 0 && v->env == 0) {
		v->wave = W_OFF; /* percussive note fully decayed before its hold ran out */
		return 0;
	}

	int32_t o = (s * (v->env >> 9)) >> 15;
	return (o * v->vol) >> 15;
}

/* ---- SFX ---------------------------------------------------------------- */

static voice_t *sfx_voice(void)
{
	for (unsigned i = V_MUSIC; i < V_TOTAL; i++) {
		if (g.v[i].wave == W_OFF) {
			return &g.v[i];
		}
	}
#if TR_AUDIO_V3
	voice_t *old = &g.v[V_MUSIC]; /* all busy: steal the oldest allocation */
	for (unsigned i = V_MUSIC + 1u; i < V_TOTAL; i++) {
		if (g.alloc_seq - g.v[i].seq > g.alloc_seq - old->seq) {
			old = &g.v[i];
		}
	}
	return old;
#else
	g.steal = (g.steal + 1u) % V_SFX; /* all busy: steal round-robin */
	return &g.v[V_MUSIC + g.steal];
#endif
}

static void sfx(const note_t *n)
{
	voice_t *v = sfx_voice();
	play(v, n, false);
	v->seq   = ++g.alloc_seq;
	v->owner = g.ev_serial;
}

unsigned tr_audio_event_voices(uint32_t serial)
{
	unsigned c = 0;
	for (unsigned k = V_MUSIC; k < V_TOTAL; k++) {
		c += g.v[k].wave != W_OFF && g.v[k].owner == serial;
	}
	return c;
}

static void sfx_crackle(unsigned bursts, uint32_t span_ms, int32_t vol, unsigned intensity)
{
	for (unsigned i = 0; i < bursts; i++) {
		note_t n = {
			.wave = W_NOISE, .filt = F_HP, .cut0 = CUT(500), .d_ms = (uint16_t)(6u + rnd() % 14u)
		};
		n.hold_ms  = n.d_ms;
		n.delay_ms = (uint16_t)(rnd() % span_ms);
		n.vol      = (int16_t)((vol + (int32_t)(rnd() % 1200u)) * (int32_t)intensity / 255);
		sfx(&n);
	}
}

static void sfx_wire(unsigned intensity)
{
	if (intensity == 0u) {
		return;
	}
	sfx(&(note_t){ .wave    = W_SAW,
	               .filt    = F_LP,
	               .cut0    = V3(CUT(120), CUT(350)),
	               .hz0     = V3(HZ(100), HZ(500)),
	               .a_ms    = 40,
	               .sus     = 100,
	               .hold_ms = 380,
	               .r_ms    = 60,
	               .vol     = (int16_t)(1800 * (int32_t)intensity / 255) });
	sfx(&(note_t){ .wave    = W_SQUARE,
	               .duty    = 64,
	               .filt    = F_LP,
	               .cut0    = V3(CUT(80), CUT(300)),
	               .hz0     = V3(HZ(150), HZ(750)),
	               .a_ms    = 40,
	               .sus     = 100,
	               .hold_ms = 380,
	               .r_ms    = 60,
	               .vol     = (int16_t)(800 * (int32_t)intensity / 255) });
	sfx_crackle(5, 380, 1100, intensity);
}

static void sfx_event(uint8_t kind, uint8_t param)
{
	switch (kind) {
	case TR_AEV_FOOTSTEP:
		sfx(&(note_t){ .wave    = W_NOISE,
		               .filt    = F_HP,
		               .cut0    = param ? CUT(550) : CUT(450),
		               .d_ms    = 28,
		               .hold_ms = 28,
		               .vol     = 1300 });
		sfx(&(note_t){ .wave    = W_SINE,
		               .hz0     = V3(HZ(95), HZ(900)),
		               .hz1     = V3(HZ(60), HZ(600)),
		               .d_ms    = V3(40, 15),
		               .hold_ms = V3(40, 15),
		               .vol     = V3(1200, 900) });
		break;
	case TR_AEV_PICKUP: {
		unsigned base = 88u + (param < 12u ? param : 12u); /* E6, a semitone up per combo step */
		for (unsigned i = 0; i < (param >= 2u ? 3u : 2u); i++) {
			static const uint8_t k_iv[3] = { 0, 7, 12 };
			sfx(&(note_t){ .wave     = W_SINE,
			               .hz0      = midi_q8(base + k_iv[i]),
			               .a_ms     = 2,
			               .d_ms     = 220,
			               .hold_ms  = 220,
			               .r_ms     = 40,
			               .delay_ms = (uint16_t)(70u * i),
			               .vol      = 2800 });
		}
		break;
	}
	case TR_AEV_JUMP:
		sfx(&(note_t){ .wave    = W_NOISE,
		               .filt    = F_LP,
		               .cut0    = CUT(40),
		               .cut1    = V2(CUT(450), CUT(250)),
		               .a_ms    = 60,
		               .sus     = 100,
		               .hold_ms = 200,
		               .r_ms    = 80,
		               .vol     = 3200 });
		sfx(&(note_t){ .wave    = W_SQUARE,
		               .filt    = F_LP,
		               .cut0    = CUT(300),
		               .hz0     = V3(HZ(260), HZ(600)),
		               .hz1     = V3(HZ(620), HZ(1100)),
		               .a_ms    = 2,
		               .d_ms    = 140,
		               .hold_ms = 140,
		               .r_ms    = 20,
		               .vol     = 1100 });
		break;
	case TR_AEV_DUCK:
		sfx(&(note_t){ .wave    = W_NOISE,
		               .filt    = F_LP,
		               .cut0    = CUT(450),
		               .cut1    = CUT(30),
		               .a_ms    = 10,
		               .sus     = 100,
		               .hold_ms = 180,
		               .r_ms    = 60,
		               .vol     = 2800 });
		sfx(&(note_t){ .wave    = W_TRI,
		               .hz0     = V3(HZ(520), HZ(900)),
		               .hz1     = V3(HZ(180), HZ(500)),
		               .d_ms    = 180,
		               .hold_ms = 180,
		               .vol     = 1500 });
		break;
	case TR_AEV_CRASH:
		g.duck = MS(1200);
		sfx(&(note_t){ .wave    = W_SINE,
		               .hz0     = V3(HZ(110), HZ(1000)),
		               .hz1     = V3(HZ(34), HZ(600)),
		               .d_ms    = V3(450, 250),
		               .hold_ms = V3(450, 250),
		               .r_ms    = 30,
		               .vol     = V2(7000, 5000) });
		sfx(&(note_t){ .wave    = V3(W_SQUARE, W_SAW),
		               .filt    = F_LP,
		               .cut0    = V3(CUT(100), CUT(400)),
		               .hz0     = V3(HZ(70), HZ(700)),
		               .hz1     = V3(HZ(40), HZ(500)),
		               .d_ms    = 400,
		               .hold_ms = 400,
		               .vol     = V2(2500, 1800) });
		sfx(&(note_t){ .wave    = W_NOISE,
		               .filt    = F_LP,
		               .cut0    = CUT(700),
		               .cut1    = CUT(20),
		               .d_ms    = 700,
		               .hold_ms = 700,
		               .vol     = V2(4500, 3200) });
		for (unsigned i = 0; i < 6u; i++) { /* debris */
			/* one rnd() per statement: initializer order is unspecified in C */
			bool     sq = (rnd() & 1u) != 0u;
			uint32_t hz = 700u + rnd() % 2600u;
			uint32_t d  = 25u + rnd() % 40u;
			uint32_t at = 80u + rnd() % 120u + 110u * i;
			note_t   n  = { .wave     = sq ? W_SQUARE : W_NOISE,
				            .filt     = sq ? V2(F_NONE, F_LP) : F_HP,
				            .cut0     = CUT(300),
				            .hz0      = sq ? HZ(hz) : 0u,
				            .d_ms     = (uint16_t)d,
				            .hold_ms  = (uint16_t)d,
				            .delay_ms = (uint16_t)at,
				            .vol      = (int16_t)((2000 - 230 * (int32_t)i) * V2(10, 7) / 10) };
			sfx(&n);
		}
		if (param == TR_CRASH_KIND_WIRE) {
			sfx(&(note_t){ .wave    = W_SAW,
			               .filt    = F_HP,
			               .cut0    = CUT(200),
			               .hz0     = HZ(1400),
			               .hz1     = V3(HZ(90), HZ(500)),
			               .d_ms    = 260,
			               .hold_ms = 260,
			               .vol     = V2(3500, 2500) });
			sfx_crackle(4, 500, 1400, 255);
		}
		break;
	case TR_AEV_WIRE:
		sfx_wire(param);
		break;
	case TR_AEV_ATTRACT: {
		static const uint8_t k_notes[5] = { 72, 76, 79, 84, 88 }; /* C5 E5 G5 C6 E6 */
		for (unsigned i = 0; i < 5u; i++) {
			bool last = i == 4u;
			sfx(&(note_t){ .wave     = W_SQUARE,
			               .duty     = 32,
			               .filt     = F_LP,
			               .cut0     = CUT(400),
			               .hz0      = midi_q8(k_notes[i]),
			               .a_ms     = 2,
			               .d_ms     = 260,
			               .sus      = last ? 40 : 0,
			               .hold_ms  = last ? 600 : 260,
			               .r_ms     = 120,
			               .delay_ms = (uint16_t)(90u * i),
			               .vol      = 2600 });
		}
		sfx(&(note_t){ .wave     = W_TRI,
		               .hz0      = midi_q8(76),
		               .a_ms     = 10,
		               .d_ms     = 300,
		               .sus      = 50,
		               .hold_ms  = 700,
		               .r_ms     = 200,
		               .delay_ms = 360,
		               .vol      = 1800 });
		break;
	}
	case TR_AEV_GAME_OVER: {
		static const uint8_t k_notes[4] = { 69, 68, 67, 66 }; /* A4 G#4 G4 F#4, falling */
		g.duck                          = MS(2000);
		for (unsigned i = 0; i < 4u; i++) {
			bool last = i == 3u;
			sfx(&(note_t){
			    .wave = W_SAW,
			    .filt = F_LP,
			    .cut0 = CUT(250),
			    .hz0  = midi_q8(k_notes[i] + V3(0u, 12u)), /* V3: A5.. above the speakers' floor */
			    .a_ms = 5,
			    .d_ms = last ? 300 : 200,
			    .sus  = last ? 70 : 30,
			    .hold_ms  = last ? 900 : 220,
			    .r_ms     = last ? 400 : 40,
			    .delay_ms = (uint16_t)(250u * i),
			    .vol      = 3000 });
		}
		sfx(&(note_t){ .wave     = W_TRI,
		               .hz0      = midi_q8(V3(57u, 81u)),
		               .a_ms     = 10,
		               .d_ms     = 400,
		               .sus      = 60,
		               .hold_ms  = 1000,
		               .r_ms     = 400,
		               .delay_ms = 750,
		               .vol      = 2200 });
		break;
	}
	default:
		break;
	}
}

/* ---- music -------------------------------------------------------------- */

/* Am F C G, twice per 8 bars: bass root (octave 2/3) and arp triad. */
static const uint8_t k_bass_root[4] = { 45, 41, 48, 43 };
static const uint8_t k_triad[4][3]  = { { 69, 72, 76 },
	                                    { 65, 69, 72 },
	                                    { 67, 72, 76 },
	                                    { 67, 71, 74 } };
/* bass: 8ths on the root, octave jumps on steps 6 and 14 */
static const int8_t k_bass_pat[16] = { 0, -1, 0, -1, 0, -1, 12, -1, 0, -1, 0, -1, 0, -1, 12, -1 };

typedef struct {
	uint8_t bar, step, note, len; /* len in sixteenths */
} lead_t;

/* Lead over bars 8..15 (0-based). */
static const lead_t k_lead[] = {
	{ 8, 0, 76, 6 },   { 8, 6, 74, 2 },   { 8, 8, 72, 4 },   { 8, 12, 71, 2 },  { 8, 14, 69, 2 },
	{ 9, 0, 72, 8 },   { 9, 8, 69, 4 },   { 9, 12, 72, 4 },  { 10, 0, 76, 4 },  { 10, 4, 79, 4 },
	{ 10, 8, 76, 4 },  { 10, 12, 74, 4 }, { 11, 0, 74, 12 }, { 11, 12, 71, 4 }, { 12, 0, 81, 6 },
	{ 12, 6, 79, 2 },  { 12, 8, 76, 8 },  { 13, 0, 77, 4 },  { 13, 4, 76, 4 },  { 13, 8, 72, 8 },
	{ 14, 0, 76, 4 },  { 14, 4, 79, 4 },  { 14, 8, 84, 8 },  { 15, 0, 83, 8 },  { 15, 8, 86, 4 },
	{ 15, 12, 83, 4 },
};

#define STEP_MS (60000u / TR_AUDIO_BPM / 4u) /* 150 ms */

static void music_step(void)
{
	uint32_t bar = g.step / 16u, st = g.step % 16u, ch = bar % 4u;
	voice_t *v = g.v;

	if (k_bass_pat[st] >= 0) {
		play(
		    &v[M_BASS],
		    &(note_t){
		        .wave = W_SAW,
		        .filt = F_LP,
		        .cut0 = V3(
		            CUT(180),
		            CUT(500)), /* V3: an upper-harmonic stack; the 450 Hz high-pass takes the fundamental */
		        .hz0     = midi_q8(k_bass_root[ch] + (unsigned)k_bass_pat[st] + V3(0u, 12u)),
		        .a_ms    = 2,
		        .d_ms    = 120,
		        .sus     = 40,
		        .hold_ms = STEP_MS,
		        .r_ms    = 40,
		        .vol     = 4500 },
		    true);
	}
	unsigned oct = bar >= 12u ? 12u : 0u;
	unsigned ai  = st % 4u;
	play(&v[M_ARP],
	     &(note_t){ .wave    = W_SQUARE,
	                .duty    = 64,
	                .filt    = F_LP,
	                .cut0    = CUT(350),
	                .hz0     = midi_q8((ai == 3u ? k_triad[ch][0] + 12u : k_triad[ch][ai]) + oct +
	                                   V3(0u, 12u)),
	                .a_ms    = 1,
	                .d_ms    = 80,
	                .hold_ms = STEP_MS,
	                .r_ms    = 30,
	                .vol     = 2000 },
	     true);
	for (unsigned i = 0; i < sizeof(k_lead) / sizeof(k_lead[0]); i++) {
		if (k_lead[i].bar == bar && k_lead[i].step == st) {
			play(&v[M_LEAD],
			     &(note_t){ .wave    = W_SAW,
			                .filt    = F_LP,
			                .cut0    = CUT(300),
			                .hz0     = midi_q8(k_lead[i].note),
			                .a_ms    = 15,
			                .d_ms    = 200,
			                .sus     = 60,
			                .hold_ms = (uint16_t)(STEP_MS * k_lead[i].len),
			                .r_ms    = 120,
			                .vol     = 2600 },
			     true);
		}
	}
	if (bar >= 4u && st % 4u == 0u) {
		play(&v[M_KICK],
		     &(note_t){ .wave    = W_SINE,
		                .hz0     = V3(HZ(150), HZ(900)),
		                .hz1     = V3(HZ(45), HZ(550)),
		                .d_ms    = V3(250, 80),
		                .hold_ms = V3(250, 80),
		                .vol     = 6000 },
		     false);
	}
	if (bar >= 4u && (st == 4u || st == 12u || (bar == 15u && st >= 13u))) {
		play(&v[M_SNARE],
		     &(note_t){ .wave    = W_NOISE,
		                .filt    = F_HP,
		                .cut0    = CUT(250),
		                .d_ms    = 140,
		                .hold_ms = 140,
		                .vol     = (int16_t)(st >= 13u ? 2400 : 3200) },
		     false);
	}
	if (st % 4u == 2u || (bar >= 12u && (st & 1u))) {
		play(&v[M_HAT],
		     &(note_t){ .wave    = W_NOISE,
		                .filt    = F_HP,
		                .cut0    = CUT(700),
		                .d_ms    = 35,
		                .hold_ms = 35,
		                .vol     = (int16_t)(st & 1u ? 600 : 1000) },
		     false);
	}
}

/* ---- API ---------------------------------------------------------------- */

void tr_audio_init(uint32_t seed)
{
	memset(&g, 0, sizeof(g));
	g.rng   = seed ? seed : 0x2545F491u;
	g.k_inc = (uint32_t)((1ull << 32) / TR_AUDIO_RATE);
}

uint32_t tr_audio_event(uint8_t kind, uint8_t param)
{
	g.ev_serial++;
	if (kind != TR_AEV_MUSIC) {
		sfx_event(kind, param);
		return g.ev_serial;
	}
	g.mode   = param <= TR_MUSIC_PLAY ? param : TR_MUSIC_PLAY;
	g.target = g.mode == TR_MUSIC_PLAY ? GAIN_PLAY : g.mode == TR_MUSIC_ATTRACT ? GAIN_ATTRACT : 0;
	if (g.mode != TR_MUSIC_OFF && !g.playing) {
		g.playing  = true;
		g.step     = 0u;
		g.step_pos = 0u;
	}
	return g.ev_serial;
}

void tr_audio_render(int16_t *buf, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		int32_t m = 0, s = 0;

		if (g.playing) {
			if (g.step_pos == 0u) {
				music_step();
			}
			if (++g.step_pos == TR_AUDIO_STEP) {
				g.step_pos = 0u;
				g.step     = (g.step + 1u) % TR_AUDIO_SONG_STEPS;
			}
			for (unsigned k = 0; k < V_MUSIC; k++) {
				if (g.v[k].wave != W_OFF) {
					m += voice_tick(&g.v[k]);
				}
			}
			int32_t t = g.duck ? g.target * DUCK_PCT / 100 : g.target;
			if (g.gain < t) {
				g.gain = g.gain + GAIN_RAMP < t ? g.gain + GAIN_RAMP : t;
			} else if (g.gain > t) {
				g.gain = g.gain - GAIN_RAMP > t ? g.gain - GAIN_RAMP : t;
			}
			if (g.gain == 0 && t == 0) {
				g.playing = false; /* faded out: stop the sequencer, silence the band */
				for (unsigned k = 0; k < V_MUSIC; k++) {
					g.v[k].wave = W_OFF;
				}
			}
		}
		if (g.duck != 0u) {
			g.duck--;
		}
		for (unsigned k = V_MUSIC; k < V_TOTAL; k++) {
			if (g.v[k].wave != W_OFF) {
				s += voice_tick(&g.v[k]);
			}
		}

		int64_t pre = (((int64_t)m * g.gain) >> 15) + s;
#if TR_AUDIO_V3
		pre = tr_audio_hpf(&g.hpf, (int32_t)pre); /* nothing the speakers cannot play */
#endif
		int64_t mix = pre * MASTER_Q8 >> 8;
#if TR_AUDIO_V2
		/* Soft knee above -2.5 dBFS: y = K + d R / (d + R), asymptote
		 * 32767, so a pile-up of effects bends instead of flat-topping. */
		int64_t a = mix < 0 ? -mix : mix;
		if (a > LIM_KNEE) {
			uint32_t d = (uint32_t)(a - LIM_KNEE > 262143 ? 262143 : a - LIM_KNEE);
			int64_t  y = LIM_KNEE + (int64_t)(d * (uint32_t)LIM_RANGE / (d + (uint32_t)LIM_RANGE));
			mix        = mix < 0 ? -y : y;
			g.limited++;
		}
#endif
		if (mix > 32767 || mix < -32767) {
			g.clipped++;
			mix = mix > 0 ? 32767 : -32767;
		}
		buf[i] = (int16_t)mix;
	}
}

uint32_t tr_audio_clipped(void)
{
	return g.clipped;
}

uint32_t tr_audio_limited(void)
{
	return g.limited;
}

void tr_audio_seek(uint32_t step)
{
	g.step     = step % TR_AUDIO_SONG_STEPS;
	g.step_pos = 0u;
}

unsigned tr_audio_sfx_active(void)
{
	unsigned c = 0;

	for (unsigned k = V_MUSIC; k < V_TOTAL; k++) {
		c += g.v[k].wave != W_OFF;
	}
	return c;
}

uint32_t tr_audio_song_step(void)
{
	return g.step;
}
