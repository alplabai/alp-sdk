/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file alp_soc_cpus.h
 * @brief Internal: render CONFIG_ALP_SDK_SOC_CPUS with the "(active)" marker
 *        on the core this image actually runs on (#2469).
 *
 * alp_orchestrate.py emits the core list from the SoC spec, marking the core
 * the app's board.yaml slice names -- e.g.
 * "M55-HP @400MHz (active)|rtss_hp + 2x Cortex-A32 @800MHz + M55-HE @160MHz|rtss_he".
 * That slice is whatever `--core` the example's CMakeLists passes, not the
 * board target the image is built for, so an HE build of an app whose
 * board.yaml only declares m55_hp was announced as running on the HP.
 *
 * Each core with a Zephyr CPU cluster carries a `|<cluster>` tag.  When one
 * tag equals the last segment of CONFIG_BOARD_TARGET, that core is marked
 * active and listed first; the orchestrator's own marker is dropped.  When
 * none matches (native_sim, a board without clusters) the orchestrator's
 * marking stands.  Tags are never printed.
 */

#ifndef ALP_SOC_CPUS_H_
#define ALP_SOC_CPUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define ALP_SOC_CPUS_ACTIVE " (active)"

/* One " + "-separated entry of the core list: [text, text + text_len) is
 * what gets printed, [tag, tag + tag_len) the optional cluster tag. */
struct alp_soc_cpu_part {
	const char *text;
	size_t      text_len;
	const char *tag;
	size_t      tag_len;
};

static inline const char *alp_soc_cpus_next(const char *s, struct alp_soc_cpu_part *p)
{
	const char *end = strstr(s, " + ");
	size_t      len = (end != NULL) ? (size_t)(end - s) : strlen(s);
	const char *bar = memchr(s, '|', len);

	p->text     = s;
	p->text_len = (bar != NULL) ? (size_t)(bar - s) : len;
	p->tag      = (bar != NULL) ? bar + 1 : NULL;
	p->tag_len  = (bar != NULL) ? len - p->text_len - 1 : 0;
	return (end != NULL) ? end + 3 : NULL;
}

static inline bool alp_soc_cpus_tag_is(const struct alp_soc_cpu_part *p, const char *cluster)
{
	return p->tag != NULL && p->tag_len == strlen(cluster) &&
	       memcmp(p->tag, cluster, p->tag_len) == 0;
}

/* Text without the orchestrator's trailing " (active)", if it has one. */
static inline size_t alp_soc_cpus_label_len(const struct alp_soc_cpu_part *p)
{
	size_t m = sizeof(ALP_SOC_CPUS_ACTIVE) - 1;

	if (p->text_len >= m && memcmp(p->text + p->text_len - m, ALP_SOC_CPUS_ACTIVE, m) == 0) {
		return p->text_len - m;
	}
	return p->text_len;
}

static inline void alp_soc_cpus_append(char *buf, size_t n, size_t *pos, const char *s, size_t len)
{
	while (len-- > 0 && *pos + 1 < n) {
		buf[(*pos)++] = *s++;
	}
	buf[*pos] = '\0';
}

/**
 * @brief Format @p cpus into @p buf (NUL-terminated, truncated to @p n),
 *        marking the core whose cluster tag equals @p board_target's last
 *        '/' segment as active.
 */
static inline void
alp_soc_cpus_format(char *buf, size_t n, const char *cpus, const char *board_target)
{
	const char             *slash   = strrchr(board_target, '/');
	const char             *cluster = (slash != NULL) ? slash + 1 : board_target;
	struct alp_soc_cpu_part p;
	const char             *s;
	bool                    matched = false;
	size_t                  pos     = 0;

	if (n == 0) {
		return;
	}
	buf[0] = '\0';

	for (s = cpus; s != NULL;) {
		s = alp_soc_cpus_next(s, &p);
		matched |= alp_soc_cpus_tag_is(&p, cluster);
	}

	if (matched) {
		for (s = cpus; s != NULL;) {
			s = alp_soc_cpus_next(s, &p);
			if (alp_soc_cpus_tag_is(&p, cluster)) {
				alp_soc_cpus_append(buf, n, &pos, p.text, alp_soc_cpus_label_len(&p));
				alp_soc_cpus_append(
				    buf, n, &pos, ALP_SOC_CPUS_ACTIVE, sizeof(ALP_SOC_CPUS_ACTIVE) - 1);
			}
		}
	}
	for (s = cpus; s != NULL;) {
		s = alp_soc_cpus_next(s, &p);
		if (matched && alp_soc_cpus_tag_is(&p, cluster)) {
			continue;
		}
		if (pos > 0) {
			alp_soc_cpus_append(buf, n, &pos, " + ", 3);
		}
		alp_soc_cpus_append(
		    buf, n, &pos, p.text, matched ? alp_soc_cpus_label_len(&p) : p.text_len);
	}
}

#endif /* ALP_SOC_CPUS_H_ */
