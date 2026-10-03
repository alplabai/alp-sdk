/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file amp_beacon.h
 * @brief AMP peer liveness beacon + attach epoch: the one definition shared
 *        by the Linux-side RPC backend and every image the peer core runs.
 *
 * In an asymmetric multiprocessing (AMP) split, the core that owns the
 * shared window publishes four 32-bit words in the last 16 bytes of the
 * rsctbl page (the page that holds the OpenAMP resource table).  The
 * other side reads them to tell a running peer from a dead one, and an
 * RPC-serving image from an idle one, without any transport being up.
 * The words are plain memory: no peripheral, interrupt or IPC is needed to
 * write them.
 *
 * Offsets are given from the END of the rsctbl page so the layout does not
 * depend on where the page sits or how big it is.  Where it sits is
 * hardware, and lives once in the SoC metadata (`openamp_carveout.regions.
 * rsctbl`, metadata/socs/<vendor>/<family>/<part>.json).
 *
 *   end-0x10  magic       ALP_AMP_BEACON_MAGIC, written LAST, so a reader that
 *                         sees it also sees the other words of this boot
 *   end-0x0C  version     < ALP_AMP_BEACON_VERSION_NO_RPC: RPC firmware beacon
 *                         version (1; 2 = attach reset supported).
 *                         >= ALP_AMP_BEACON_VERSION_NO_RPC: image that serves
 *                         no RPC (ALP_AMP_BEACON_VERSION_IDLE_SHIM)
 *   end-0x08  heartbeat   ~1 Hz counter; growth between two reads = alive
 *   end-0x04  epoch       (version >= 2) ODD = bound to a peer session (set
 *                         after the transport is up), EVEN = waiting for an
 *                         attach; starts at 0
 *
 * The page contents survive a reset of the publishing core, so a publisher
 * zeroes the heartbeat and epoch and writes the magic last on every boot.
 */

#ifndef ALP_PROTOCOL_AMP_BEACON_H
#define ALP_PROTOCOL_AMP_BEACON_H

#include <stdint.h>

#define ALP_AMP_BEACON_MAGIC 0xA10D0683u /* "Alp Lab, #683" */

/** First beacon version whose firmware acknowledges an attach reset. */
#define ALP_AMP_BEACON_VERSION_ATTACH_ACK 2u
/** Versions from here up are images that serve no RPC. */
#define ALP_AMP_BEACON_VERSION_NO_RPC 0x100u
/** Version published by the idle stock shim (firmware/alp-stock-shim). */
#define ALP_AMP_BEACON_VERSION_IDLE_SHIM 0x100u

/** Beacon size in bytes: the four words above. */
#define ALP_AMP_BEACON_SIZE 0x10u

/** Byte offset of each word inside an rsctbl page of `page_size` bytes. */
#define ALP_AMP_BEACON_MAGIC_OFF(page_size)     ((page_size) - 0x10u)
#define ALP_AMP_BEACON_VERSION_OFF(page_size)   ((page_size) - 0x0Cu)
#define ALP_AMP_BEACON_HEARTBEAT_OFF(page_size) ((page_size) - 0x08u)
#define ALP_AMP_BEACON_EPOCH_OFF(page_size)     ((page_size) - 0x04u)

/** The same four words as a struct, for the publishing side. */
struct alp_amp_beacon {
	uint32_t magic;
	uint32_t version;
	uint32_t heartbeat;
	uint32_t attach_epoch;
};

_Static_assert(sizeof(struct alp_amp_beacon) == ALP_AMP_BEACON_SIZE,
               "alp_amp_beacon must be exactly the 16 bytes the A55 side reads");

/** Beacon of the rsctbl page starting at `base` and `page_size` bytes long. */
#define ALP_AMP_BEACON_AT(base, page_size) \
	((volatile struct alp_amp_beacon *)((uintptr_t)(base) + (page_size) - \
	                                    sizeof(struct alp_amp_beacon)))

#endif /* ALP_PROTOCOL_AMP_BEACON_H */
