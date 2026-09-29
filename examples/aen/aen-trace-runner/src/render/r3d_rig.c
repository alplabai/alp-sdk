/* src/render/r3d_rig.c -- see r3d_rig.h. */
#include "r3d_rig.h"

#include "../game/state.h"
#include "meshes.h"

#include <string.h>

_Static_assert(TR_ANIM_CYCLE == TR_RUN_CYCLE_TICKS, "genmesh.py ANIM_CYCLE != state.h TR_RUN_CYCLE_TICKS");
_Static_assert(TR_ANIM_CH == 25 && TR_ANIM_SIDE0 == 9 && TR_ANIM_SIDE_N == 8, "r3d_rig.h drifted from meshes.h");
_Static_assert(TR_RIG_FACE == TR_RIG_PARTS, "r3d_rig.h drifted");

#define PI_F 3.14159265f
#define RAD  (180.0f / PI_F)

/* Per-side channel offsets (genmesh.py S_*). */
#define S_THIGH 0
#define S_KNEE  1
#define S_FOOT  2
#define S_SPLAY 3

/* 3x4 row-major affine. */
typedef struct {
	float m[12];
} m34_t;

/* o = a . b */
static m34_t mmul(const m34_t *a, const m34_t *b)
{
	m34_t o;

	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			float v = a->m[r * 4 + 0] * b->m[0 * 4 + c] + a->m[r * 4 + 1] * b->m[1 * 4 + c] +
				  a->m[r * 4 + 2] * b->m[2 * 4 + c];

			o.m[r * 4 + c] = c == 3 ? v + a->m[r * 4 + 3] : v;
		}
	}
	return o;
}

/* m . R(axis, deg): x 0, y 1, z 2 -- genmesh.py RX / RY / RZ. */
static void rotate(m34_t *m, int axis, float deg)
{
	float s, c;
	m34_t r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};

	tr_sincosf(deg * (PI_F / 180.0f), &s, &c);
	if (axis == 0) {
		r.m[5] = c, r.m[6] = -s, r.m[9] = s, r.m[10] = c;
	} else if (axis == 1) {
		r.m[0] = c, r.m[2] = s, r.m[8] = -s, r.m[10] = c;
	} else {
		r.m[0] = c, r.m[1] = -s, r.m[4] = s, r.m[5] = c;
	}
	*m = mmul(m, &r);
}

/* Skinning matrix per bone: world(bone) . T(-bind joint). genmesh.py
 * rig_mats() is the mirror the run cycle was designed through. */
/* nsc[b]: undoes a mid-joint bone's scale on normals (those bones have no
 * children, so nothing else inherits it). */
static void rig_bones(const tr_rig_bone_t *bone, const float ch[TR_ANIM_CH], m34_t skin[TR_RIG_BONES],
		      float nsc[TR_RIG_BONES])
{
	m34_t world[TR_RIG_BONES];

	for (int b = 0; b < TR_RIG_BONES; b++) {
		const tr_rig_bone_t *d  = &bone[b];
		float                px = 0.0f, py = 0.0f, pz = 0.0f;

		if (d->parent >= 0) {
			px = bone[d->parent].jx, py = bone[d->parent].jy, pz = bone[d->parent].jz;
		}
		m34_t l = {{1, 0, 0, d->jx - px, 0, 1, 0, d->jy - py + (d->parent < 0 ? ch[TR_ANIM_ROOT_Y] : 0.0f), 0, 0,
			    1, d->jz - pz}};

		for (int k = 0; k < d->nrot; k++) {
			rotate(&l, d->axis[k], ch[d->ch[k]] * d->k[k]);
		}
		nsc[b] = 1.0f;
		if (d->sc_ch >= 0) {
			float s, c;

			tr_sincosf(ch[d->sc_ch] * d->sc_k * (PI_F / 180.0f), &s, &c);
			for (int r = 0; r < 3; r++) {
				for (int k = 0; k < 3; k++) {
					l.m[r * 4 + k] = l.m[r * 4 + k] / c;
				}
			}
			nsc[b] = c;
		}
		world[b] = d->parent >= 0 ? mmul(&world[d->parent], &l) : l;
		skin[b]  = world[b];
		for (int r = 0; r < 3; r++) {
			skin[b].m[r * 4 + 3] = world[b].m[r * 4 + 3] - (world[b].m[r * 4 + 0] * d->jx +
									world[b].m[r * 4 + 1] * d->jy +
									world[b].m[r * 4 + 2] * d->jz);
		}
	}
}

void tr_rig_run(float cycle, float ch[TR_ANIM_CH], float lock)
{
	float c1, s1, cs[TR_ANIM_H + 1], sn[TR_ANIM_H + 1];
	float frac = cycle - (float)(int32_t)cycle;

	if (frac < 0.0f) {
		frac += 1.0f;
	}
	tr_sincosf(2.0f * PI_F * frac, &s1, &c1);
	cs[0] = 1.0f, sn[0] = 0.0f;
	for (int k = 1; k <= TR_ANIM_H; k++) { /* angle addition: cos/sin of k theta */
		cs[k] = cs[k - 1] * c1 - sn[k - 1] * s1;
		sn[k] = sn[k - 1] * c1 + cs[k - 1] * s1;
	}
	for (int c = 0; c < TR_ANIM_CH; c++) {
		/* right side: the left's channel half a stride on, where
		 * cos/sin(k (theta + pi)) = (-1)^k cos/sin(k theta) */
		int          right = c >= TR_ANIM_SIDE0 + TR_ANIM_SIDE_N;
		const float *a     = tr_anim_run[right ? c - TR_ANIM_SIDE_N : c];
		float        v     = a[0];

		for (int k = 1; k <= TR_ANIM_H; k++) {
			float sg = (right && (k & 1)) ? -1.0f : 1.0f;

			v += sg * (a[2 * k - 1] * cs[k] + a[2 * k] * sn[k]);
		}
		ch[c] = v;
	}
	tr_rig_foot_lock(cycle, ch, lock);
}

/* Rotation part of m applied to p, and its transpose. */
static void rot_ap(const m34_t *m, const float p[3], float o[3])
{
	for (int k = 0; k < 3; k++) {
		o[k] = m->m[k * 4 + 0] * p[0] + m->m[k * 4 + 1] * p[1] + m->m[k * 4 + 2] * p[2];
	}
}

static void rot_apt(const m34_t *m, const float p[3], float o[3])
{
	for (int k = 0; k < 3; k++) {
		o[k] = m->m[0 * 4 + k] * p[0] + m->m[1 * 4 + k] * p[1] + m->m[2 * 4 + k] * p[2];
	}
}

/* Thigh s's frame before its splay and swing, at the hip joint: the pelvis
 * (root, yaw, pitch, roll) and the thigh's counter-yaw -- rig_bones()'s
 * chain for that bone, as genmesh.py _hip_frame(). */
static m34_t hip_frame(const float ch[TR_ANIM_CH], int s)
{
	m34_t m = {{1, 0, 0, 0, 0, 1, 0, TR_RIG_HIP_Y + ch[TR_ANIM_ROOT_Y], 0, 0, 1, 0}};
	m34_t t = {{1, 0, 0, s ? TR_RIG_HIP_X : -TR_RIG_HIP_X, 0, 1, 0, 0, 0, 0, 1, 0}};

	rotate(&m, 1, ch[TR_ANIM_P_YAW]);
	rotate(&m, 0, ch[TR_ANIM_P_PITCH]);
	rotate(&m, 2, ch[TR_ANIM_P_ROLL]);
	m = mmul(&m, &t);
	rotate(&m, 1, -ch[TR_ANIM_P_YAW]);
	return m;
}

/*
 * Side s's splay / thigh / knee / foot channels so the ball of its shoe
 * (TR_ANIM_BALL_*) lands on P (runner local) with the shoe toe-down `a`
 * degrees: the splay turns the leg's plane through the ankle target, then
 * the 2-bone IK in that plane (law of cosines, knee bending forward), and
 * the foot takes up the rest of the pitch. The ball offset turns with the
 * splay, so that is iterated three times (settled after two). Mirror: genmesh.py leg_ik().
 */
static void leg_ik(float ch[TR_ANIM_CH], int s, const float P[3], float a)
{
	static const float ball[3] = {0.0f, TR_ANIM_BALL_Y, TR_ANIM_BALL_Z};
	const float        l1 = TR_RIG_THIGH_L, l2 = TR_RIG_SHIN_L;
	int                c  = TR_ANIM_SIDE0 + TR_ANIM_SIDE_N * s;
	float              sx = s ? 1.0f : -1.0f;
	m34_t              w0 = hip_frame(ch, s);
	float              spl = ch[c + S_SPLAY] * sx, d[3] = {0.0f, -1.0f, 0.0f}, sr, cr, yy, zz, dd, ck, sk, th;

	for (int it = 0; it < 3; it++) {
		m34_t f = w0;
		float b[3], q[3];

		rotate(&f, 2, spl);
		rotate(&f, 0, a - ch[TR_ANIM_P_PITCH]);
		rot_ap(&f, ball, b);
		for (int k = 0; k < 3; k++) {
			q[k] = P[k] - b[k] - w0.m[k * 4 + 3];
		}
		rot_apt(&w0, q, d);
		spl = tr_atan2f(d[0], -d[1]) * RAD;
	}
	tr_sincosf(spl / RAD, &sr, &cr);
	yy = -sr * d[0] + cr * d[1];
	zz = d[2];
	dd = __builtin_sqrtf(yy * yy + zz * zz);
	dd = dd < l1 - l2 + 0.5f ? l1 - l2 + 0.5f : dd; /* l1 > l2 */
	dd = dd > TR_ANIM_REACH * (l1 + l2) ? TR_ANIM_REACH * (l1 + l2) : dd;
	ck = (dd * dd - l1 * l1 - l2 * l2) / (2.0f * l1 * l2);
	ck = ck > 1.0f ? 1.0f : (ck < -1.0f ? -1.0f : ck);
	sk = __builtin_sqrtf(1.0f - ck * ck);
	th = tr_atan2f(-zz, -yy) - tr_atan2f(l2 * sk, l1 + l2 * ck);
	ch[c + S_SPLAY] = spl * sx;
	ch[c + S_THIGH] = th * RAD;
	ch[c + S_KNEE]  = tr_atan2f(sk, ck) * RAD;
	ch[c + S_FOOT]  = a - ch[TR_ANIM_P_PITCH] - ch[c + S_THIGH] - ch[c + S_KNEE];
}

void tr_rig_foot_lock(float cycle, float ch[TR_ANIM_CH], float w)
{
	for (int s = 0; s < 2 && w > 0.0f; s++) {
		float u = cycle + 0.5f * (float)s, lw = 1.0f, us, t, off = 0.0f, P[3], ik[TR_ANIM_CH];
		int   c = TR_ANIM_SIDE0 + TR_ANIM_SIDE_N * s;

		u -= (float)(int32_t)u;
		u += u < 0.0f ? 1.0f : 0.0f;
		us = u;
		if (u >= TR_ANIM_STANCE) { /* off the board: easing out after toe-off, in before touch-down */
			float da = u - TR_ANIM_STANCE, db = 1.0f - u, x;

			us  = da < db ? u : u - 1.0f;
			off = (da < db ? da : db) / TR_ANIM_LOCK_BLEND;
			x   = 1.0f - off;
			x  = x < 0.0f ? 0.0f : x;
			lw = x * x * (3.0f - 2.0f * x);
		}
		lw *= w;
		if (lw <= 0.0f) {
			continue;
		}
		t    = us / TR_ANIM_STANCE;
		t    = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		P[0] = s ? TR_ANIM_FOOT_X : -TR_ANIM_FOOT_X;
		P[1] = TR_ANIM_LOCK_LIFT * off * off; /* off the board: rising clear of it */
		P[2] = TR_ANIM_TD_Z - TR_ANIM_GROUND_V * (float)TR_ANIM_CYCLE * us;
		memcpy(ik, ch, sizeof(ik));
		leg_ik(ik, s, P, TR_ANIM_PITCH_TD + (TR_ANIM_PITCH_TO - TR_ANIM_PITCH_TD) * t * t);
		for (int k = 0; k < 4; k++) {
			ch[c + k] += (ik[c + k] - ch[c + k]) * lw;
		}
	}
}

const float *tr_rig_pose(int which)
{
	return tr_anim_pose[which];
}

int tr_rig_char_id(int chr)
{
	return chr >= 0 && chr < TR_CHARS ? chr : 0;
}

const tr_rig_char_t *tr_rig_char(int chr)
{
	return &tr_rig_chars[tr_rig_char_id(chr)];
}

void tr_rig_points(int chr, const float ch[TR_ANIM_CH], int b, int n, const float (*p)[3], float (*out)[3])
{
	m34_t        skin[TR_RIG_BONES];
	float        nsc[TR_RIG_BONES];
	const float *s;

	rig_bones(tr_rig_chars[tr_rig_char_id(chr)].bone, ch, skin, nsc);
	s = skin[b].m;
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < 3; k++) {
			out[i][k] = s[k * 4 + 0] * p[i][0] + s[k * 4 + 1] * p[i][1] + s[k * 4 + 2] * p[i][2] + s[k * 4 + 3];
		}
	}
}

void tr_rig_joint(int chr, const float ch[TR_ANIM_CH], int b, float out[3])
{
	const tr_rig_bone_t *d = &tr_rig_chars[tr_rig_char_id(chr)].bone[b];
	const float          j[3] = {d->jx, d->jy, d->jz};

	tr_rig_points(chr, ch, d->parent >= 0 ? d->parent : b, 1, &j, (float (*)[3])out);
}

static const uint8_t face_bone[TR_FACE_V] = {TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD, TR_RIG_HEAD};

static int lod_of(int lod)
{
	return lod > 0 && lod < TR_RIG_LODS ? lod : 0;
}

const uint8_t *tr_rig_bones(int chr, int lod, int part)
{
	if (part == TR_RIG_FACE) {
		return face_bone;
	}
	return tr_rig_chars[tr_rig_char_id(chr)].bones[lod_of(lod)][part];
}

static int8_t q8(float v)
{
	float r = v * 127.0f;

	r = r > 127.0f ? 127.0f : (r < -127.0f ? -127.0f : r);
	return (int8_t)(int32_t)(r + (r >= 0.0f ? 0.5f : -0.5f));
}

/*
 * The eyes (P16), in the head's bind space: per eye TR_FACE_COLS columns
 * (outer edge ... inner edge, mirrored) of a bottom and a top vertex, laid
 * on the visor -- the head's ellipsoid (face[3] radius z; 0: a flat visor
 * at z = face[4]) 1.6 units proud. A rounded lens when open; the squint
 * drops the top (the inner corner most) and lifts the bottom; happy bends
 * the band into an arc (middle high, edges low); a blink closes it onto its
 * middle line. Never thinner than 0.14 of the eye's height: a shut eye is
 * a line. Quads between neighbouring columns, 2 x (TR_FACE_COLS - 1) each.
 */
static const uint8_t face_tri[TR_FACE_T * 3] = {0, 1, 6, 0, 6, 5, 1, 2, 7, 1, 7, 6, 2, 3, 8, 2, 8, 7, 3, 4, 9, 3, 9, 8, 10, 11, 16, 10, 16, 15, 11, 12, 17, 11, 17, 16, 12, 13, 18, 12, 18, 17, 13, 14, 19, 13, 19, 18};
static const int8_t  face_n[TR_FACE_T * 3]   = {0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127};
static const uint8_t face_col[TR_FACE_T]     = {TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE, TR_CHAR_EYE};
static const int8_t  eye_vn[TR_FACE_V * 3]   = {0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127};
_Static_assert(TR_FACE_COLS == 5, "face_tri / lens / arch are written for 5 columns");
static const float   lens[TR_FACE_COLS] = {0.55f, 0.9f, 1.0f, 0.9f, 0.55f}; /* half height per column */
static const float   arch[TR_FACE_COLS] = {0.1f, 0.62f, 0.9f, 0.62f, 0.1f}; /* happy: the arc's top */

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void face_shape(const float *fc, const tr_face_t *f, float v[TR_FACE_V * 3])
{
	static const tr_face_t neutral = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	float                  sz, W, H, open, sq, hp;

	f    = f ? f : &neutral;
	open = clamp01(f->open), sq = clamp01(f->squint), hp = clamp01(f->happy);
	sz   = 1.0f + 0.3f * clamp01(f->wide);
	W = fc[7] * sz, H = fc[8] * sz;
	for (int e = 0; e < 2; e++) {
		float sx = e ? 1.0f : -1.0f;

		for (int j = 0; j < TR_FACE_COLS; j++) {
			float u     = (float)j / (float)(TR_FACE_COLS - 1); /* 0 .. 1 across */
			float prof  = lens[j], inner = e ? 1.0f - u : u;          /* the inner corner: toward the nose */
			float top = H * prof - sq * (0.55f * H * prof + 0.35f * H * inner), bot = -H * prof + sq * 0.25f * H * prof;
			float ah = H * arch[j], mid, x, y;

			top = top + (ah - top) * hp;
			bot = bot + (ah - 0.55f * H - bot) * hp;
			mid = 0.5f * (top + bot);
			top = mid + (top - mid) * open;
			bot = mid + (bot - mid) * open;
			if (top - bot < 0.14f * H) {
				top = mid + 0.07f * H, bot = mid - 0.07f * H;
			}
			x = sx * fc[6] + f->look_x + (2.0f * u - 1.0f) * W;
			for (int t = 0; t < 2; t++) {
				float *q = &v[(e * 2 * TR_FACE_COLS + t * TR_FACE_COLS + j) * 3], z;

				y = fc[5] + f->look_y + (t ? top : bot);
				if (fc[3] > 0.0f) {
					float r = 1.0f - (x / fc[1]) * (x / fc[1]) - (y / fc[2]) * (y / fc[2]);

					z = fc[3] * __builtin_sqrtf(r > 0.05f ? r : 0.05f) + 1.6f;
				} else {
					z = fc[4] + 0.6f;
				}
				q[0] = x, q[1] = fc[0] + y, q[2] = z;
			}
		}
	}
}

void tr_rig_skin(int chr, int lod, const float ch[TR_ANIM_CH], const tr_face_t *face,
		 float xyz[TR_RIG_DRAWN][TR_RIG_MAX_V * 3], int8_t vn[TR_RIG_DRAWN][TR_RIG_MAX_V * 3],
		 tr_mesh_t out[TR_RIG_DRAWN])
{
	const tr_rig_char_t *c = &tr_rig_chars[tr_rig_char_id(chr)];
	m34_t                skin[TR_RIG_BONES];
	float                nsc[TR_RIG_BONES], eye[TR_FACE_V * 3];

	lod = lod_of(lod);
	rig_bones(c->bone, ch, skin, nsc);
	for (int p = 0; p < TR_RIG_PARTS; p++) {
		out[p] = *c->mesh[lod][p];
	}
	face_shape(c->face, face, eye);
	out[TR_RIG_FACE] = (tr_mesh_t){NULL, face_tri, face_n, face_col, TR_FACE_V, TR_FACE_T, eye_vn, NULL,
				       c->mesh[lod][0]->pal, c->mesh[lod][0]->emis};
	for (int p = 0; p < TR_RIG_DRAWN; p++) {
		const tr_mesh_t *m  = &out[p];
		const uint8_t   *bn = tr_rig_bones(chr, lod, p);

		for (uint16_t i = 0; i < m->nv; i++) {
			const float *s = skin[bn[i]].m;
			float        x = p == TR_RIG_FACE ? eye[i * 3 + 0] : (float)m->v[i * 3 + 0];
			float        y = p == TR_RIG_FACE ? eye[i * 3 + 1] : (float)m->v[i * 3 + 1];
			float        z = p == TR_RIG_FACE ? eye[i * 3 + 2] : (float)m->v[i * 3 + 2];
			float        nx = (float)m->vn[i * 3 + 0] / 127.0f, ny = (float)m->vn[i * 3 + 1] / 127.0f;
			float        nz = (float)m->vn[i * 3 + 2] / 127.0f;
			float        r[3];

			for (int k = 0; k < 3; k++) {
				xyz[p][i * 3 + k] = s[k * 4 + 0] * x + s[k * 4 + 1] * y + s[k * 4 + 2] * z + s[k * 4 + 3];
				r[k]              = s[k * 4 + 0] * nx + s[k * 4 + 1] * ny + s[k * 4 + 2] * nz;
				vn[p][i * 3 + k]  = q8(r[k] * nsc[bn[i]]);
			}
		}
		out[p].vf = xyz[p];
		out[p].vn = vn[p];
	}
}
