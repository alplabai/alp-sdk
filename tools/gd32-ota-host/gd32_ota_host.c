/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32_ota_host -- drive the GD32 bridge A/B OTA from Linux userspace
 * (E1M-X V2N A55) through the portable alp-sdk API: gd32g553_ota_begin /
 * write_chunk / verify / commit over the yocto i2c-dev backend.  This is
 * the alp-sdk host-path proof for issue #86 (the firmware side is proven
 * in alplabai/gd32-bridge-firmware#167).
 *
 * The kernel gpio-gd32-bridge driver binds the bridge's I2C address, and
 * i2c-dev refuses I2C_SLAVE on a bound address (EBUSY).  --unbind detaches
 * that driver through sysfs for the run (so its polling cannot interleave
 * with OTA frames) and re-binds it at exit.
 *
 * Usage: see README.md.  Exit 0 = every requested phase passed.
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alp/chips/gd32g553.h"
#include "alp/peripheral.h"

/* Chunk data ceiling: the wire allows MAX_PAYLOAD (65) - offset(4) - len(1) = 60, but the
 * firmware needs 8-aligned offsets, so every non-final chunk is a multiple of 8. */
#define WIRE_CHUNK_MAX 56u

static uint32_t crc32_zlib(const uint8_t *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

static const char *ota_state_name(gd32g553_ota_state_t s)
{
	static const char *n[] = { "IDLE", "READY", "BUSY", "VERIFIED", "ERROR" };
	return (unsigned)s < 5u ? n[s] : "?";
}

/* ---- sysfs unbind/rebind of the kernel driver holding the address ---- */

static char g_drv[64], g_dev[32], g_bind[128];

static int sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	size_t n  = strlen(val);
	int    rc = (write(fd, val, n) == (ssize_t)n) ? 0 : -1;
	if (close(fd) != 0) rc = -1;
	return rc;
}

static void kernel_unbind(unsigned bus, unsigned addr)
{
	char link[128], tgt[256], path[256];
	/* Block the signals across the detach so a signal cannot land between the
	 * sysfs write and the state on_signal() needs to undo it. */
	sigset_t all, old;
	sigemptyset(&all);
	sigaddset(&all, SIGINT);
	sigaddset(&all, SIGTERM);
	sigaddset(&all, SIGHUP);
	sigprocmask(SIG_BLOCK, &all, &old);
	snprintf(g_dev, sizeof(g_dev), "%u-%04x", bus, addr);
	snprintf(link, sizeof(link), "/sys/bus/i2c/devices/%s/driver", g_dev);
	ssize_t n = readlink(link, tgt, sizeof(tgt) - 1);
	if (n < 0) {
		printf("unbind: %s has no kernel driver bound\n", g_dev);
		g_dev[0] = 0;
		sigprocmask(SIG_SETMASK, &old, NULL);
		return;
	}
	tgt[n] = 0;
	snprintf(g_drv, sizeof(g_drv), "%s", basename(tgt));
	snprintf(g_bind, sizeof(g_bind), "/sys/bus/i2c/drivers/%s/bind", g_drv);
	snprintf(path, sizeof(path), "/sys/bus/i2c/drivers/%s/unbind", g_drv);
	if (sysfs_write(path, g_dev) != 0) {
		printf("unbind: %s from %s -> FAILED\n", g_dev, g_drv);
		g_dev[0] = 0; /* nothing was detached, so nothing to re-bind */
	} else {
		printf("unbind: %s from %s -> ok\n", g_dev, g_drv);
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
}

/* SIGINT/SIGTERM/SIGHUP mid-run: re-bind (async-signal-safe calls only) so the
 * bridge is not left without its kernel driver. */
static void on_signal(int sig)
{
	if (g_dev[0] != 0) (void)sysfs_write(g_bind, g_dev);
	_exit(128 + sig);
}

static void kernel_rebind(void)
{
	if (g_dev[0] == 0) return;
	printf("rebind: %s to %s -> %s\n",
	       g_dev,
	       g_drv,
	       sysfs_write(g_bind, g_dev) == 0 ? "ok" : "FAILED");
	g_dev[0] = 0;
}

/* ---- OTA phases ---- */

static alp_i2c_t *g_i2c;

static int open_link(gd32g553_t *g, unsigned bus, unsigned addr)
{
	if (g_i2c == NULL) {
		alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(bus);
		g_i2c                = alp_i2c_open(&cfg);
		if (g_i2c == NULL) {
			fprintf(stderr,
			        "FAIL: alp_i2c_open(/dev/i2c-%u) last_error=%d\n",
			        bus,
			        (int)alp_last_error());
			return 1;
		}
	}
	alp_status_t s = gd32g553_init(g, NULL, g_i2c, (uint8_t)addr);
	if (s != ALP_OK) {
		fprintf(stderr, "init: status=%d\n", (int)s);
		return 1;
	}
	return 0;
}

static void print_link(gd32g553_t *g, const char *tag)
{
	char                      id[GD32G553_BUILD_ID_LEN + 1] = { 0 };
	gd32g553_ota_state_info_t st;
	alp_status_t              sb = gd32g553_get_build_id(g, id);
	alp_status_t              ss = gd32g553_ota_get_state(g, &st);
	printf("%s: protocol=%u.%u.%u build_id=%s",
	       tag,
	       g->version.major,
	       g->version.minor,
	       g->version.patch,
	       sb == ALP_OK ? id : "(err)");
	if (ss == ALP_OK)
		printf(" state=%s active_slot=%u pending_slot=%u boot_count=%u err=0x%02x\n",
		       ota_state_name(st.state),
		       (unsigned)st.active_slot,
		       (unsigned)st.pending_slot,
		       st.boot_count,
		       (unsigned)st.err);
	else
		printf(" get_state status=%d\n", (int)ss);
}

/* Build id + OTA state in one go; 0 only when BOTH reads succeeded. */
static int
snapshot(gd32g553_t *g, char id[GD32G553_BUILD_ID_LEN + 1], gd32g553_ota_state_info_t *st)
{
	memset(id, 0, GD32G553_BUILD_ID_LEN + 1);
	return gd32g553_get_build_id(g, id) == ALP_OK && gd32g553_ota_get_state(g, st) == ALP_OK ? 0
	                                                                                         : 1;
}

/* Poll GET_STATE until state == want (BEGIN's erase / VERIFY's CRC run in
 * the background or block the reply; ALP_ERR_IO/BUSY mean "still busy"). */
static int wait_state(gd32g553_t *g, gd32g553_ota_state_t want, unsigned timeout_ms)
{
	for (unsigned t = 0; t <= timeout_ms; t += 50) {
		gd32g553_ota_state_info_t st;
		if (gd32g553_ota_get_state(g, &st) == ALP_OK) {
			if (st.state == want) return 0;
			if (st.state == GD32G553_OTA_STATE_ERROR) {
				fprintf(stderr, "bridge OTA state ERROR err=0x%02x\n", (unsigned)st.err);
				return 1;
			}
		}
		alp_delay_ms(50);
	}
	fprintf(stderr, "timeout waiting for state %s\n", ota_state_name(want));
	return 1;
}

static int usage(void)
{
	fprintf(stderr,
	        "usage: gd32_ota_host --image slot.bin --version M.m.p [--bus 8] [--addr 0x70]\n"
	        "                     [--unbind] [--no-commit] [--status]\n");
	return 2;
}

int main(int argc, char **argv)
{
	const char *image = NULL;
	unsigned    bus = 8, addr = 0x70, vm = 0, vn = 0, vp = 0;
	int         have_ver = 0, do_unbind = 0, do_commit = 1, status_only = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--image") && i + 1 < argc)
			image = argv[++i];
		else if (!strcmp(argv[i], "--version") && i + 1 < argc)
			have_ver = sscanf(argv[++i], "%u.%u.%u", &vm, &vn, &vp) == 3;
		else if (!strcmp(argv[i], "--bus") && i + 1 < argc)
			bus = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			addr = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--unbind"))
			do_unbind = 1;
		else if (!strcmp(argv[i], "--no-commit"))
			do_commit = 0;
		else if (!strcmp(argv[i], "--status"))
			status_only = 1;
		else
			return usage();
	}
	if (!status_only && (image == NULL || !have_ver || vm > 255 || vn > 255 || vp > 255))
		return usage();

	uint8_t *buf = NULL;
	size_t   len = 0;
	if (!status_only) {
		FILE *f = fopen(image, "rb");
		if (f == NULL) {
			fprintf(stderr, "FAIL: open %s: %s\n", image, strerror(errno));
			return 1;
		}
		fseek(f, 0, SEEK_END);
		len = (size_t)ftell(f);
		rewind(f);
		buf = malloc(len ? len : 1);
		if (buf == NULL || len == 0 || fread(buf, 1, len, f) != len) {
			fprintf(stderr, "FAIL: read %s (empty or short)\n", image);
			fclose(f);
			free(buf);
			return 1;
		}
		fclose(f);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	if (do_unbind) kernel_unbind(bus, addr);

	int        rc = 1;
	gd32g553_t g;
	if (open_link(&g, bus, addr)) goto out;
	print_link(&g, "before");
	char                      before_id[GD32G553_BUILD_ID_LEN + 1];
	gd32g553_ota_state_info_t before;
	if (snapshot(&g, before_id, &before)) {
		fprintf(stderr, "FAIL: cannot read build id / OTA state before the run\n");
		goto out;
	}
	if (status_only) {
		rc = 0;
		goto out;
	}

	const uint32_t crc = crc32_zlib(buf, len);
	printf("image: %s size=%zu crc32=0x%08x version=%u.%u.%u\n", image, len, crc, vm, vn, vp);

	const gd32g553_version_t fv         = { (uint8_t)vm, (uint8_t)vn, (uint8_t)vp };
	uint16_t                 chunk_max  = 0;
	gd32g553_ota_slot_t      slot       = GD32G553_OTA_SLOT_NONE;
	int                      lost_begin = 0;
	alp_status_t             s = gd32g553_ota_begin(&g, (uint32_t)len, crc, &fv, &chunk_max, &slot);
	if (s == ALP_ERR_IO || s == ALP_ERR_TIMEOUT) {
		/* BEGIN's reply can be lost to the slot erase: confirm via state. */
		printf("BEGIN reply lost (status=%d), confirming via GET_STATE\n", (int)s);
		lost_begin = 1;
	} else if (s != ALP_OK) {
		fprintf(stderr, "FAIL: BEGIN status=%d\n", (int)s);
		goto out;
	} else {
		printf("BEGIN ok: chunk_max=%u target_slot=%u\n", chunk_max, (unsigned)slot);
	}
	if (wait_state(&g, GD32G553_OTA_STATE_READY, 30000)) goto out;
	if (lost_begin) {
		/* READY may be a stale session from an earlier abort: only trust it
		 * when it is staging into the slot we are not running. */
		gd32g553_ota_state_info_t st;
		if (gd32g553_ota_get_state(&g, &st) != ALP_OK ||
		    st.pending_slot == GD32G553_OTA_SLOT_NONE || st.pending_slot == before.active_slot) {
			fprintf(stderr,
			        "FAIL: lost BEGIN reply and pending slot is not a fresh staging slot\n");
			goto out;
		}
		slot = st.pending_slot;
		printf("BEGIN confirmed: target_slot=%u\n", (unsigned)slot);
	}
	if (slot == before.active_slot) {
		fprintf(stderr, "FAIL: BEGIN target slot equals the running slot\n");
		goto out;
	}

	/* chunk_max stays 0 after a lost BEGIN reply: fall back to the wire ceiling.  Every
	 * non-final chunk must be a multiple of 8 so the next offset stays 8-aligned. */
	size_t step = (chunk_max != 0 && chunk_max < WIRE_CHUNK_MAX) ? chunk_max : WIRE_CHUNK_MAX;
	step &= ~(size_t)7;
	if (step < 8) step = 8;

	uint32_t got = 0;
	for (size_t off = 0; off < len;) {
		size_t n     = len - off < step ? len - off : step;
		int    tries = 0;
		for (;;) {
			s = gd32g553_ota_write_chunk(&g, (uint32_t)off, buf + off, n, &got);
			if (s == ALP_OK && got >= off + n) break;
			if (++tries <= 2) {
				alp_delay_ms(20);
				continue;
			}
			/* gd32-bridge-firmware#315: a BRD_I2C slave at 0x55 holds SDA low
			 * after certain byte sequences, so the same frame times out every
			 * time.  The same bytes framed at a different length pass: re-send
			 * this chunk as a shorter 8-aligned piece, then resume full size. */
			if (n <= 8) {
				fprintf(stderr, "FAIL: chunk @%zu status=%d got=%u\n", off, (int)s, got);
				goto out;
			}
			n     = ((n / 2) + 7) & ~(size_t)7;
			tries = 0;
			printf("chunk @%zu: stalled, retrying as %zu bytes\n", off, n);
		}
		/* The reply's high-water is authoritative (only read after s == ALP_OK):
		 * if an earlier lost-reply attempt already landed more than this piece,
		 * resume from there instead of overlapping written data. */
		off += (got > off + n && got <= len && (got % 8 == 0 || got == len)) ? got - off : n;
	}
	printf("WRITE ok: %zu bytes, high-water=%u, chunk=%zu\n", len, got, step);

	bool     ok   = false;
	uint32_t vcrc = 0;
	s             = gd32g553_ota_verify(&g, &ok, &vcrc);
	if (s != ALP_OK && s != ALP_ERR_IO && s != ALP_ERR_TIMEOUT) {
		fprintf(stderr, "FAIL: VERIFY status=%d\n", (int)s);
		goto out;
	}
	if (s != ALP_OK) {
		printf("VERIFY reply lost (status=%d), confirming via GET_STATE\n", (int)s);
		if (wait_state(&g, GD32G553_OTA_STATE_VERIFIED, 30000)) goto out;
		printf("VERIFY: ok (state VERIFIED)\n");
	} else {
		printf("VERIFY: %s computed_crc32=0x%08x expected=0x%08x\n",
		       ok ? "ok" : "MISMATCH",
		       vcrc,
		       crc);
		if (!ok || vcrc != crc) goto out;
	}

	if (!do_commit) {
		rc = 0;
		goto out;
	}

	s = gd32g553_ota_commit(&g);
	printf("COMMIT: status=%d (IO/TIMEOUT expected: bridge resets before the reply)\n", (int)s);
	if (s != ALP_OK && s != ALP_ERR_IO && s != ALP_ERR_TIMEOUT) goto out;

	/* Bridge resets twice (commit + trial confirm); re-init rides out BUSY. */
	int up = 0;
	for (int i = 0; i < 30 && !up; i++) {
		alp_delay_ms(500);
		up = (open_link(&g, bus, addr) == 0);
	}
	if (!up) {
		fprintf(stderr, "FAIL: bridge did not come back within 15 s\n");
		goto out;
	}
	print_link(&g, "after");

	/* Strict verdict: the bridge must now run the target slot, on a new build. */
	char                      after_id[GD32G553_BUILD_ID_LEN + 1];
	gd32g553_ota_state_info_t after;
	if (snapshot(&g, after_id, &after)) {
		fprintf(stderr, "FAIL: cannot read build id / OTA state after COMMIT\n");
		goto out;
	}
	if (after.state == GD32G553_OTA_STATE_ERROR) {
		fprintf(stderr, "FAIL: OTA state ERROR err=0x%02x after COMMIT\n", (unsigned)after.err);
		goto out;
	}
	if (after.active_slot != slot || after.active_slot == before.active_slot) {
		fprintf(stderr,
		        "FAIL: active slot %u (was %u), expected target %u: COMMIT rejected or reverted\n",
		        (unsigned)after.active_slot,
		        (unsigned)before.active_slot,
		        (unsigned)slot);
		goto out;
	}
	if (!strcmp(after_id, before_id)) {
		fprintf(stderr, "FAIL: build id unchanged (%s) after COMMIT\n", after_id);
		goto out;
	}
	rc = 0;
out:
	kernel_rebind();
	free(buf);
	puts(rc == 0 ? "RESULT: PASS" : "RESULT: FAIL");
	return rc;
}
