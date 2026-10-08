# Trace Runner Core Implementation Plan (M0-M3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An original endless runner on the E1M-AEN803's 720x1280 panel that a player controls by standing in front of the camera, with tilt as a second mode.

**Architecture:** Pure C game core with no hardware knowledge, host-compiled and unit-tested; a thin platform layer that is the only code touching `<alp/*>`; a renderer using erase-all/move-all/draw-all dirty rects; and a vision module that turns a person bounding box into game intents through a pure, testable mapping function.

**Tech Stack:** C11, Zephyr via alp-sdk, `<alp/display.h>`, `<alp/inference.h>`, `<alp/chips/bmi323.h>`, `<alp/peripheral.h>`, host builds with plain `cc` + assert.

**Spec:** `docs/2026-09-22-trace-runner-design.md`

## Global Constraints

- **Board targets:** `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he` and `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`.
- **Shields:** `e1m_evk_rk055hdmipi4ma0` for the panel (720x1280, `pixel-fmt-l1 = "rgb-565"`, refresh 40.0 Hz measured). From Task 8 the camera needs two more **stacked**: `-DSHIELD="e1m_evk_rk055hdmipi4ma0;e1m_evk_rpi_csi;innomaker_cam_ov9281"`. Building the camera example without its two shields fails with `#error "aen-camera-firstlight needs the innomaker_cam_ov9281 shield stacked on e1m_evk_rpi_csi"`, so this is not optional.
- **Build:** `ZEPHYR_BASE=$ZEPHYR_BASE`, `-DEXTRA_ZEPHYR_MODULES=<alp-sdk checkout>;<hal_alif>`, `-DPython3_EXECUTABLE=/usr/bin/python3`.
- **SDK checkout prerequisite:** the alp-sdk checkout must carry BOTH the camera support on `dev` (`b5cce96a1`, bench-verified OV9281 CSI-2 capture) AND the display shield from `fix/aen-dsi-display-u35-expander-address`. Merge `origin/dev` into the display branch before Task 8 and use that checkout for every build in this plan.
- **Console:** RAM console (`CONFIG_RAM_CONSOLE=y`, `CONFIG_UART_CONSOLE=n`) — this board's console FTDI passes zero bytes.
- **Style:** tabs, `clang-format-22` clean, comments explain WHY. Examples are documentation.
- **Content:** every asset original to this project. No third-party game content, no reference to any existing title, character or franchise.
- **Licence:** Apache-2.0. Consume alp-sdk through `<alp/*>` only; an SDK gap is logged against the SDK, never worked around here.
- **Hardware truths that shape the code:** panel init fails roughly 1 cold boot in 8-10 with no re-init path; touch is unusable (GT911 config block reads blank); HyperRAM is unusable (CK/CK# crossed at U9). Free memory: SRAM0 4 MB (1.84 MB is the framebuffer), SRAM1 4 MB, MRAM 5.5 MB, OSPI NOR 32 MB.
- **Load flow:** M0-M1 fit the 256 KB ITCM RAM-run. From M2 the model pushes the image past that, so M2 onward use the MRAM slot0 flash flow.
- **Commit messages:** no AI attribution, no lab-farm place names; refer to boards as SKU + serial (`a bench E1M-AEN803`).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/game/state.h` / `state.c` | Game state: lanes, runner, entities, score. Pure C, no hardware. |
| `src/game/step.c` | One tick of game logic: movement, spawning, collision, scoring. |
| `src/game/intent.h` | `tr_intent_t` — the only vocabulary between input and game. |
| `src/vision/track.h` / `track.c` | Pure mapping: a sequence of person boxes -> intents. Host-testable. |
| `src/platform/display.c` | Opens the display, validates caps, owns blits. Only file calling `<alp/display.h>`. |
| `src/platform/imu.c` | Tilt mode input via `<alp/chips/bmi323.h>`. |
| `src/platform/vision_hw.c` | Camera capture + `alp_inference_invoke()`, produces boxes for `track.c`. |
| `src/render/render.c` | Game state -> draw calls, dirty-rect discipline. |
| `src/main.c` | Wiring, mode select, main loop, frame pacing. |
| `tests/host/test_step.c` | Unit tests for game logic. |
| `tests/host/test_track.c` | Unit tests for the box -> intent mapping. |
| `tests/host/runner.sh` | Builds and runs host tests with plain `cc`. |

---

### Task 1: Host test harness and the intent vocabulary

**Files:**
- Create: `src/game/intent.h`
- Create: `tests/host/runner.sh`
- Create: `tests/host/test_intent.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `tr_intent_t` with fields `int8_t lane_delta` (-1, 0, +1), `bool jump`, `bool duck`, `tr_input_source_t source`; enum `TR_INPUT_NONE`, `TR_INPUT_TILT`, `TR_INPUT_VISION`.

- [ ] **Step 1: Write the failing test**

```c
/* tests/host/test_intent.c */
#include <assert.h>
#include "../../src/game/intent.h"

int main(void)
{
	tr_intent_t i = tr_intent_none();

	assert(i.lane_delta == 0);
	assert(!i.jump);
	assert(!i.duck);
	assert(i.source == TR_INPUT_NONE);
	return 0;
}
```

- [ ] **Step 2: Write the test runner**

```bash
#!/bin/bash
# tests/host/runner.sh -- builds and runs every host test, exits non-zero on the first failure.
set -u
cd "$(dirname "$0")/../.." || exit 1
rc=0
for t in tests/host/test_*.c; do
	out="/tmp/tr-$(basename "$t" .c)"
	if ! cc -std=c11 -Wall -Wextra -Werror -g -o "$out" "$t" $(ls src/game/*.c src/vision/*.c 2>/dev/null | grep -v main.c) -lm; then
		echo "BUILD FAIL: $t"; rc=1; continue
	fi
	if "$out"; then echo "PASS: $t"; else echo "FAIL: $t"; rc=1; fi
done
exit "$rc"
```

- [ ] **Step 3: Run it to verify it fails**

Run: `chmod +x tests/host/runner.sh && ./tests/host/runner.sh`
Expected: `BUILD FAIL` — `intent.h` does not exist.

- [ ] **Step 4: Write the minimal header**

```c
/* src/game/intent.h */
#ifndef TR_INTENT_H
#define TR_INTENT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The ONLY vocabulary between an input device and the game.  Tilt and body
 * tracking both reduce to this, so the game never learns which one is driving
 * and a new input never reaches into game state.
 */
typedef enum {
	TR_INPUT_NONE = 0,
	TR_INPUT_TILT,
	TR_INPUT_VISION,
} tr_input_source_t;

typedef struct {
	int8_t            lane_delta; /**< -1 left, 0 hold, +1 right. */
	bool              jump;
	bool              duck;
	tr_input_source_t source;
} tr_intent_t;

static inline tr_intent_t tr_intent_none(void)
{
	return (tr_intent_t){ .lane_delta = 0, .jump = false, .duck = false, .source = TR_INPUT_NONE };
}

#endif /* TR_INTENT_H */
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./tests/host/runner.sh`
Expected: `PASS: tests/host/test_intent.c`

- [ ] **Step 6: Commit**

```bash
git add src/game/intent.h tests/host/runner.sh tests/host/test_intent.c
git commit -m "feat(game): add the input intent vocabulary and a host test runner"
```

---

### Task 2: Game state and one tick of logic

**Files:**
- Create: `src/game/state.h`, `src/game/state.c`, `src/game/step.c`
- Create: `tests/host/test_step.c`

**Interfaces:**
- Consumes: `tr_intent_t` from Task 1.
- Produces:
  - `#define TR_LANES 3`, `#define TR_MAX_ENTITIES 16`
  - `typedef enum { TR_ENT_FREE, TR_ENT_OBSTACLE, TR_ENT_PICKUP } tr_entity_kind_t;`
  - `typedef struct { tr_entity_kind_t kind; uint8_t lane; int16_t y; bool low; } tr_entity_t;`
  - `typedef struct { uint8_t lane; bool airborne; uint8_t air_ticks; bool ducking; uint8_t duck_ticks; uint32_t score; bool alive; uint32_t tick; uint32_t rng; tr_entity_t ents[TR_MAX_ENTITIES]; } tr_game_t;`
  - `void tr_game_init(tr_game_t *g, uint32_t seed);`
  - `void tr_game_step(tr_game_t *g, tr_intent_t in, int16_t track_h);`

- [ ] **Step 1: Write the failing tests**

```c
/* tests/host/test_step.c */
#include <assert.h>
#include <string.h>
#include "../../src/game/state.h"

static tr_intent_t move(int8_t d)
{
	tr_intent_t i = tr_intent_none();
	i.lane_delta   = d;
	i.source       = TR_INPUT_VISION;
	return i;
}

int main(void)
{
	tr_game_t g;

	/* Starts alive, centre lane, no score. */
	tr_game_init(&g, 1234u);
	assert(g.alive && g.lane == 1 && g.score == 0);

	/* Lane changes clamp at the edges rather than wrapping. */
	tr_game_step(&g, move(-1), 1280);
	assert(g.lane == 0);
	tr_game_step(&g, move(-1), 1280);
	assert(g.lane == 0);
	tr_game_step(&g, move(+1), 1280);
	tr_game_step(&g, move(+1), 1280);
	assert(g.lane == 2);
	tr_game_step(&g, move(+1), 1280);
	assert(g.lane == 2);

	/* A jump lifts the runner for a bounded number of ticks. */
	tr_game_init(&g, 1u);
	tr_intent_t j = tr_intent_none();
	j.jump        = true;
	tr_game_step(&g, j, 1280);
	assert(g.airborne);
	for (int k = 0; k < 64; k++) {
		tr_game_step(&g, tr_intent_none(), 1280);
	}
	assert(!g.airborne);

	/* A ground obstacle in the runner's lane kills; jumping over it does not. */
	tr_game_init(&g, 7u);
	memset(g.ents, 0, sizeof(g.ents));
	g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 1, .y = 1279, .low = true };
	while (g.alive && g.ents[0].kind != TR_ENT_FREE) {
		tr_game_step(&g, tr_intent_none(), 1280);
	}
	assert(!g.alive);

	/* A pickup in the lane scores and frees its slot. */
	tr_game_init(&g, 9u);
	memset(g.ents, 0, sizeof(g.ents));
	g.ents[0] = (tr_entity_t){ .kind = TR_ENT_PICKUP, .lane = 1, .y = 1279, .low = true };
	while (g.alive && g.ents[0].kind != TR_ENT_FREE) {
		tr_game_step(&g, tr_intent_none(), 1280);
	}
	assert(g.alive && g.score > 0);

	/* The same seed replays identically -- the game is deterministic. */
	tr_game_t a, b;
	tr_game_init(&a, 42u);
	tr_game_init(&b, 42u);
	for (int k = 0; k < 500; k++) {
		tr_game_step(&a, tr_intent_none(), 1280);
		tr_game_step(&b, tr_intent_none(), 1280);
	}
	assert(a.score == b.score && a.tick == b.tick);
	return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tests/host/runner.sh`
Expected: `BUILD FAIL` — `state.h` does not exist.

- [ ] **Step 3: Write the header**

```c
/* src/game/state.h */
#ifndef TR_STATE_H
#define TR_STATE_H

#include "intent.h"

#define TR_LANES         3
#define TR_MAX_ENTITIES  16
#define TR_AIR_TICKS     14 /* ~0.5 s at 30 Hz: long enough to clear one obstacle. */
#define TR_DUCK_TICKS    12
#define TR_SCROLL_PX     18 /* Pixels per tick an entity travels down the track. */

typedef enum {
	TR_ENT_FREE = 0,
	TR_ENT_OBSTACLE,
	TR_ENT_PICKUP,
} tr_entity_kind_t;

typedef struct {
	tr_entity_kind_t kind;
	uint8_t          lane;
	int16_t          y;
	bool             low; /**< Ground-level: clear it by jumping.  Otherwise duck. */
} tr_entity_t;

typedef struct {
	uint8_t     lane;
	bool        airborne;
	uint8_t     air_ticks;
	bool        ducking;
	uint8_t     duck_ticks;
	uint32_t    score;
	bool        alive;
	uint32_t    tick;
	uint32_t    rng;
	tr_entity_t ents[TR_MAX_ENTITIES];
} tr_game_t;

void tr_game_init(tr_game_t *g, uint32_t seed);

/*
 * Advance one tick.  `track_h` is the playfield height in pixels, passed in
 * rather than hardcoded so the logic stays independent of the panel and the
 * host tests can use any geometry.
 */
void tr_game_step(tr_game_t *g, tr_intent_t in, int16_t track_h);

#endif /* TR_STATE_H */
```

- [ ] **Step 4: Write the implementation**

```c
/* src/game/state.c */
#include "state.h"

void tr_game_init(tr_game_t *g, uint32_t seed)
{
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		g->ents[i] = (tr_entity_t){ 0 };
	}
	g->lane       = TR_LANES / 2u;
	g->airborne   = false;
	g->air_ticks  = 0;
	g->ducking    = false;
	g->duck_ticks = 0;
	g->score      = 0;
	g->alive      = true;
	g->tick       = 0;
	/* Seed 0 would lock the LCG at zero for ever; fold it to a non-zero constant. */
	g->rng = seed ? seed : 0x2f6e2b1u;
}
```

```c
/* src/game/step.c */
#include "state.h"

/* Small LCG: deterministic across host and target, which is what the replay test checks. */
static uint32_t rng_next(tr_game_t *g)
{
	g->rng = g->rng * 1664525u + 1013904223u;
	return g->rng >> 8;
}

static void spawn(tr_game_t *g, int16_t track_h)
{
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		if (g->ents[i].kind != TR_ENT_FREE) {
			continue;
		}
		uint32_t r      = rng_next(g);
		g->ents[i].kind = (r & 3u) ? TR_ENT_OBSTACLE : TR_ENT_PICKUP;
		g->ents[i].lane = (uint8_t)((r >> 3) % TR_LANES);
		g->ents[i].y    = (int16_t)(track_h - 1);
		/* Two thirds of obstacles are low (jump); the rest are high (duck). */
		g->ents[i].low = ((r >> 7) % 3u) != 0u;
		return;
	}
}

void tr_game_step(tr_game_t *g, tr_intent_t in, int16_t track_h)
{
	if (!g->alive) {
		return;
	}
	g->tick++;

	/* Lane changes clamp; wrapping would let a player dodge by spamming one direction. */
	if (in.lane_delta < 0 && g->lane > 0u) {
		g->lane--;
	} else if (in.lane_delta > 0 && g->lane + 1u < TR_LANES) {
		g->lane++;
	}

	if (in.jump && !g->airborne && !g->ducking) {
		g->airborne  = true;
		g->air_ticks = TR_AIR_TICKS;
	}
	if (in.duck && !g->airborne && !g->ducking) {
		g->ducking    = true;
		g->duck_ticks = TR_DUCK_TICKS;
	}
	if (g->airborne && --g->air_ticks == 0u) {
		g->airborne = false;
	}
	if (g->ducking && --g->duck_ticks == 0u) {
		g->ducking = false;
	}

	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		tr_entity_t *e = &g->ents[i];

		if (e->kind == TR_ENT_FREE) {
			continue;
		}
		e->y -= TR_SCROLL_PX;

		/* The runner occupies the band [0, TR_SCROLL_PX): one tick's travel. */
		bool at_runner = (e->y < TR_SCROLL_PX) && (e->y > -TR_SCROLL_PX);

		if (at_runner && e->lane == g->lane) {
			if (e->kind == TR_ENT_PICKUP) {
				g->score += 10u;
				e->kind = TR_ENT_FREE;
				continue;
			}
			/* Low obstacles are cleared by being airborne, high ones by ducking. */
			bool cleared = e->low ? g->airborne : g->ducking;

			if (!cleared) {
				g->alive = false;
				return;
			}
		}
		if (e->y <= -TR_SCROLL_PX) {
			e->kind = TR_ENT_FREE;
			g->score += 1u; /* Surviving past one costs nothing but is worth something. */
		}
	}

	if ((g->tick % 12u) == 0u) {
		spawn(g, track_h);
	}
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./tests/host/runner.sh`
Expected: `PASS` for both test files.

- [ ] **Step 6: Commit**

```bash
git add src/game tests/host/test_step.c
git commit -m "feat(game): add deterministic game state and tick logic with host tests"
```

---

### Task 3: Zephyr app skeleton that opens the display

**Files:**
- Create: `CMakeLists.txt`, `prj.conf`, `src/main.c`
- Create: `boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay`
- Create: `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`
- Create: `src/platform/display.c`, `src/platform/display.h`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `int tr_display_open(void);` returns 0 on success, negative on failure
  - `uint16_t tr_display_width(void);` / `tr_display_height(void);`
  - `int tr_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *px);`
  - `int tr_display_clear(void);`

- [ ] **Step 1: Write the platform display layer**

```c
/* src/platform/display.h */
#ifndef TR_PLATFORM_DISPLAY_H
#define TR_PLATFORM_DISPLAY_H

#include <stdint.h>

int      tr_display_open(void);
uint16_t tr_display_width(void);
uint16_t tr_display_height(void);
int      tr_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *px);
int      tr_display_clear(void);

#endif /* TR_PLATFORM_DISPLAY_H */
```

```c
/* src/platform/display.c */
#include <alp/display.h>
#include <alp/peripheral.h>
#include <zephyr/sys/printk.h>

#include "display.h"

/*
 * The ONLY file in this project that talks to the display API.  Everything
 * else works in game coordinates, so swapping panels or SDK versions touches
 * exactly one file.
 */
static alp_display_t     *g_disp;
static alp_display_caps_t g_caps;
static alp_status_t       g_first_err = ALP_OK;

int tr_display_open(void)
{
	alp_display_config_t cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);

	g_disp = alp_display_open(&cfg);
	if (g_disp == NULL) {
		/*
		 * The panel's init fails on roughly 1 cold boot in 8-10 and Zephyr has
		 * no re-init path, so say so plainly and let the caller exit rather
		 * than spinning on a dead device.
		 */
		printk("RESULT FAIL: display did not open -- known intermittent panel "
		       "init defect, power-cycle and retry\n");
		return -1;
	}
	if (alp_display_get_caps(g_disp, &g_caps) != ALP_OK) {
		printk("RESULT FAIL: display caps unavailable\n");
		return -1;
	}
	if (g_caps.format != ALP_PIXFMT_RGB565) {
		/* Every draw path below assumes 2 bytes per pixel. */
		printk("RESULT FAIL: expected ALP_PIXFMT_RGB565 (%d), panel reports %d\n",
		       (int)ALP_PIXFMT_RGB565, (int)g_caps.format);
		return -1;
	}
	printk("display : %ux%u RGB565\n", g_caps.width, g_caps.height);
	return 0;
}

uint16_t tr_display_width(void)
{
	return g_caps.width;
}

uint16_t tr_display_height(void)
{
	return g_caps.height;
}

int tr_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *px)
{
	alp_status_t st = alp_display_blit(g_disp, x, y, w, h, px);

	if (st != ALP_OK && g_first_err == ALP_OK) {
		/* Latch the first error only: a per-frame print would bury the boot log. */
		g_first_err = st;
		printk("display: first blit error status=%d at (%u,%u,%ux%u)\n", (int)st, x, y, w, h);
	}
	return (st == ALP_OK) ? 0 : -1;
}

int tr_display_clear(void)
{
	return (alp_display_clear(g_disp) == ALP_OK) ? 0 : -1;
}
```

- [ ] **Step 2: Write the build files**

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/game/state.c
	src/game/step.c
	src/platform/display.c
)
target_include_directories(app PRIVATE src)
```

```
# prj.conf
CONFIG_ALP_SDK=y

# This board's console FTDI passes zero bytes, so the bench reads ram_console_buf
# over SWD.  Keep this identical to the display example so one reader works for both.
CONFIG_RAM_CONSOLE=y
CONFIG_RAM_CONSOLE_BUFFER_SIZE=8192
CONFIG_UART_CONSOLE=n
CONFIG_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_LOG=n
CONFIG_MAIN_STACK_SIZE=8192
```

Copy both board overlays verbatim from `examples/aen/aen-dsi-display/boards/` in the alp-sdk checkout; they retarget the image into ITCM for the RAM-run flow and are not project-specific.

- [ ] **Step 3: Write a main that proves the display opens**

```c
/* src/main.c */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "platform/display.h"

int main(void)
{
	printk("\n=== trace-runner ===\n");

	if (tr_display_open() != 0) {
		return 0;
	}
	if (tr_display_clear() != 0) {
		printk("RESULT FAIL: clear failed\n");
		return 0;
	}
	printk("RESULT PASS: display open, %ux%u -- no gameplay yet\n", tr_display_width(),
	       tr_display_height());
	return 0;
}
```

- [ ] **Step 4: Build for both targets**

Run, from the project root with `<sdk>` as the alp-sdk checkout path:

```bash
ZEPHYR_BASE=$ZEPHYR_BASE west build -p always -d build/803 \
  -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he . -- \
  -DSHIELD=e1m_evk_rk055hdmipi4ma0 \
  "-DEXTRA_ZEPHYR_MODULES=<sdk>;<hal_alif>" \
  -DPython3_EXECUTABLE=/usr/bin/python3
```

Expected: clean build, and the same command with `alp_e1m_aen801_m55_he/...` into `build/801` also clean.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt prj.conf src/main.c src/platform boards
git commit -m "feat(platform): open the panel through the portable display API"
```

---

### Task 4: Renderer with the erase/move/draw invariant

**Files:**
- Create: `src/render/render.h`, `src/render/render.c`
- Modify: `src/main.c`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `tr_game_t` (Task 2), `tr_display_*` (Task 3).
- Produces:
  - `void tr_render_init(uint16_t w, uint16_t h);`
  - `void tr_render_frame(const tr_game_t *g);`
  - `#define TR_SPRITE_MAX_W 64`, `#define TR_SPRITE_MAX_H 64`

**Note:** the flat coloured rectangles here are a deliberate placeholder. They get the geometry, the pacing and the erase/draw invariant right with nothing else in the way. Task 11 replaces them with real artwork, and the demo is not presentable until it does.

- [ ] **Step 1: Write the renderer**

```c
/* src/render/render.h */
#ifndef TR_RENDER_H
#define TR_RENDER_H

#include "../game/state.h"

#define TR_SPRITE_MAX_W 64
#define TR_SPRITE_MAX_H 64

void tr_render_init(uint16_t w, uint16_t h);
void tr_render_frame(const tr_game_t *g);

#endif /* TR_RENDER_H */
```

```c
/* src/render/render.c */
#include <string.h>

#include "../platform/display.h"
#include "render.h"

/*
 * THE INVARIANT, and the one rule to keep when editing this file:
 *
 *   erase everything that moved, THEN move, THEN draw everything.
 *
 * An erase paints background over a rectangle.  If a sprite is erased but not
 * redrawn in the same frame, anything sharing those pixels is destroyed -- a
 * stationary sprite gets eaten alive by its neighbours' erases.  So the runner
 * is redrawn unconditionally every frame, even when it has not moved.
 */

#define RGB565(r, g, b) ((uint16_t)(((r) &0xF8) << 8 | ((g) &0xFC) << 3 | (b) >> 3))

#define COLOR_BG       RGB565(6, 10, 18)
#define COLOR_LANE     RGB565(24, 48, 40)
#define COLOR_RUNNER   RGB565(80, 230, 170)
#define COLOR_OBSTACLE RGB565(230, 70, 70)
#define COLOR_PICKUP   RGB565(240, 200, 60)

#define RUNNER_W 44
#define RUNNER_H 44
#define ENT_W    40
#define ENT_H    40

BUILD_ASSERT(RUNNER_W <= TR_SPRITE_MAX_W && RUNNER_H <= TR_SPRITE_MAX_H,
	     "runner sprite exceeds the scratch buffer");
BUILD_ASSERT(ENT_W <= TR_SPRITE_MAX_W && ENT_H <= TR_SPRITE_MAX_H,
	     "entity sprite exceeds the scratch buffer");

static uint16_t g_buf[TR_SPRITE_MAX_W * TR_SPRITE_MAX_H];
static uint16_t g_w, g_h;
static int16_t  g_prev_runner_x = -1, g_prev_runner_y = -1;
static int16_t  g_prev_ent_x[TR_MAX_ENTITIES], g_prev_ent_y[TR_MAX_ENTITIES];

void tr_render_init(uint16_t w, uint16_t h)
{
	g_w = w;
	g_h = h;
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		g_prev_ent_x[i] = -1;
		g_prev_ent_y[i] = -1;
	}
	g_prev_runner_x = -1;
	g_prev_runner_y = -1;
}

static void fill(uint16_t w, uint16_t h, uint16_t colour)
{
	for (unsigned i = 0; i < (unsigned)w * h; i++) {
		g_buf[i] = colour;
	}
}

static void paint(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t colour)
{
	if (x < 0 || y < 0 || x + w > g_w || y + h > g_h) {
		return; /* Never blit outside the panel: the driver would reject it anyway. */
	}
	fill(w, h, colour);
	(void)tr_display_blit((uint16_t)x, (uint16_t)y, w, h, g_buf);
}

static int16_t lane_x(uint8_t lane, uint16_t sprite_w)
{
	uint16_t band = g_w / TR_LANES;

	return (int16_t)(lane * band + (band - sprite_w) / 2u);
}

static int16_t runner_y(const tr_game_t *g)
{
	/* Airborne lifts the runner up the screen; ducking squashes it downward. */
	int16_t base = (int16_t)(g_h - RUNNER_H - 80);

	return g->airborne ? (int16_t)(base - 90) : base;
}

void tr_render_frame(const tr_game_t *g)
{
	/* 1. ERASE everything that has a previous position. */
	if (g_prev_runner_x >= 0) {
		paint(g_prev_runner_x, g_prev_runner_y, RUNNER_W, RUNNER_H, COLOR_BG);
	}
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		if (g_prev_ent_x[i] >= 0) {
			paint(g_prev_ent_x[i], g_prev_ent_y[i], ENT_W, ENT_H, COLOR_BG);
			g_prev_ent_x[i] = -1;
		}
	}

	/* 2. DRAW, recording each new position for the next frame's erase. */
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		const tr_entity_t *e = &g->ents[i];

		if (e->kind == TR_ENT_FREE) {
			continue;
		}
		int16_t x = lane_x(e->lane, ENT_W);
		int16_t y = e->y;

		if (y < 0 || y + ENT_H > g_h) {
			continue;
		}
		paint(x, y, ENT_W, ENT_H,
		      (e->kind == TR_ENT_OBSTACLE) ? COLOR_OBSTACLE : COLOR_PICKUP);
		g_prev_ent_x[i] = x;
		g_prev_ent_y[i] = y;
	}

	/* The runner is drawn unconditionally -- see the invariant at the top. */
	int16_t rx = lane_x(g->lane, RUNNER_W);
	int16_t ry = runner_y(g);

	paint(rx, ry, RUNNER_W, RUNNER_H, COLOR_RUNNER);
	g_prev_runner_x = rx;
	g_prev_runner_y = ry;
}
```

- [ ] **Step 2: Wire it into main with frame pacing**

```c
/* src/main.c -- replace the body of main() */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "game/state.h"
#include "platform/display.h"
#include "render/render.h"

#define TICK_MS 33 /* ~30 Hz logic against the panel's measured 40.0 Hz refresh. */

int main(void)
{
	printk("\n=== trace-runner ===\n");

	if (tr_display_open() != 0) {
		return 0;
	}
	(void)tr_display_clear();
	tr_render_init(tr_display_width(), tr_display_height());

	tr_game_t g;
	tr_game_init(&g, (uint32_t)k_cycle_get_32());

	uint32_t frames = 0, last_report = (uint32_t)k_uptime_get();

	for (;;) {
		int64_t start = k_uptime_get();

		tr_game_step(&g, tr_intent_none(), (int16_t)tr_display_height());
		tr_render_frame(&g);
		frames++;

		if (!g.alive) {
			printk("run over: score=%u ticks=%u\n", g.score, g.tick);
			k_msleep(1500);
			(void)tr_display_clear();
			tr_game_init(&g, (uint32_t)k_cycle_get_32());
			tr_render_init(tr_display_width(), tr_display_height());
			last_report = (uint32_t)k_uptime_get();
			frames      = 0;
		}

		uint32_t now = (uint32_t)k_uptime_get();

		if (now - last_report >= 3000u) {
			printk("fps=%u.%u score=%u\n", frames * 1000u / (now - last_report),
			       (frames * 10000u / (now - last_report)) % 10u, g.score);
			last_report = now;
			frames      = 0;
		}

		int64_t spent = k_uptime_get() - start;

		if (spent < TICK_MS) {
			k_msleep((int32_t)(TICK_MS - spent));
		}
	}
	return 0;
}
```

- [ ] **Step 3: Add the renderer to the build**

In `CMakeLists.txt`, add `src/render/render.c` to `target_sources`.

- [ ] **Step 4: Build both targets**

Run the two `west build` commands from Task 3 Step 4.
Expected: clean builds.

- [ ] **Step 5: Commit**

```bash
git add src/render src/main.c CMakeLists.txt
git commit -m "feat(render): draw the track with an erase-all, move-all, draw-all invariant"
```

---

### Task 5: Bench-verify M0 on silicon

**Files:**
- Create: `docs/bench/2026-XX-XX-m0.md` (date it the day it runs)

**Interfaces:**
- Consumes: the ELF from Task 4.
- Produces: an evidence record later tasks cite.

- [ ] **Step 1: Run it on the board**

Hand the ELF to a bench run on `a bench E1M-AEN803`, via the Flow C ITCM RAM-run, with a **cold power cycle before every boot**. The RAM console is read over SWD: resolve `ram_console_buf` and its position symbol with `arm-zephyr-eabi-nm` from this build's own `zephyr.elf`, because they move between builds.

- [ ] **Step 2: Record what was observed**

The record must contain, verbatim: the `display :` line, the `fps=` lines, the `run over:` line, and a photograph or camera frame of the glass. A build that only links is not evidence.

Expect roughly 1 boot in 8-10 to print the known panel-init failure line instead; that is the documented defect, not a regression.

- [ ] **Step 3: Commit the record**

```bash
git add docs/bench
git commit -m "docs(bench): record the M0 run on a bench E1M-AEN803"
```

---

### Task 6: Tilt mode input

**Files:**
- Create: `src/platform/imu.c`, `src/platform/imu.h`
- Modify: `src/main.c`, `CMakeLists.txt`, `prj.conf`

**Interfaces:**
- Consumes: `tr_intent_t` (Task 1).
- Produces:
  - `int tr_imu_open(void);` 0 on success, negative if absent
  - `tr_intent_t tr_imu_poll(void);`

- [ ] **Step 1: Write the IMU layer**

```c
/* src/platform/imu.h */
#ifndef TR_PLATFORM_IMU_H
#define TR_PLATFORM_IMU_H

#include "../game/intent.h"

int         tr_imu_open(void);
tr_intent_t tr_imu_poll(void);

#endif /* TR_PLATFORM_IMU_H */
```

```c
/* src/platform/imu.c */
#include <alp/chips/bmi323.h>
#include <alp/peripheral.h>
#include <zephyr/sys/printk.h>

#include "imu.h"

/*
 * Tilt is the SECOND control mode, for playing with the board in your hands.
 * The primary mode is body tracking (see vision/), because a player standing in
 * front of the camera cannot reach the board to tilt it.
 *
 * The BMI323 is on the CARRIER (EVK U13), not the SoM.  Bus and address come
 * from metadata/boards/e1m-evk.yaml's BMI323 entry.
 */

/* Q8 fixed point throughout: this runs every tick and the FPU is not enabled. */
#define STEER_DEAD_ZONE_Q8 15  /* ~0.06 g: ignore hand tremor. */
#define STEER_EDGE_Q8      90  /* ~0.35 g: past this, commit to a lane change. */
#define IMU_FAIL_LIMIT     5

static bmi323_t g_imu;
static alp_i2c_t *g_bus;
static bool       g_ok;
static unsigned   g_fails;
static bool       g_latched; /* One lane change per tilt gesture, not per tick. */

int tr_imu_open(void)
{
	alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(ALP_E1M_I2C0);

	g_bus = alp_i2c_open(&cfg);
	if (g_bus == NULL) {
		printk("imu     : bus unavailable -- tilt mode disabled\n");
		return -1;
	}
	if (bmi323_init(&g_imu, g_bus, EVK_I2C_ADDR_BMI323) != ALP_OK) {
		printk("imu     : BMI323 init failed -- tilt mode disabled\n");
		return -1;
	}
	if (bmi323_set_accel(&g_imu, BMI323_ODR_100HZ, BMI323_ACCEL_FS_2G) != ALP_OK) {
		printk("imu     : accel config failed -- tilt mode disabled\n");
		return -1;
	}
	g_ok = true;
	printk("imu     : READY (tilt mode available)\n");
	return 0;
}

tr_intent_t tr_imu_poll(void)
{
	tr_intent_t out = tr_intent_none();

	if (!g_ok) {
		return out;
	}

	bmi323_axes_t a;

	if (bmi323_read_accel(&g_imu, &a) != ALP_OK) {
		/* Coast over one bad read; give up after several so a wedged bus cannot
		 * blow the frame budget every tick for the rest of the run. */
		if (++g_fails >= IMU_FAIL_LIMIT) {
			g_ok = false;
			printk("imu     : %u consecutive read failures -- tilt mode off\n", g_fails);
		}
		return out;
	}
	g_fails = 0;

	/* 2 g full scale over int16: divide by 128 to land in Q8 g. */
	int32_t tilt_q8 = (int32_t)a.x / 128;

	if (tilt_q8 > -STEER_DEAD_ZONE_Q8 && tilt_q8 < STEER_DEAD_ZONE_Q8) {
		g_latched = false; /* Back to level: the next tilt may fire again. */
		return out;
	}
	if (!g_latched) {
		if (tilt_q8 <= -STEER_EDGE_Q8) {
			out.lane_delta = -1;
			g_latched      = true;
		} else if (tilt_q8 >= STEER_EDGE_Q8) {
			out.lane_delta = +1;
			g_latched      = true;
		}
	}
	out.source = TR_INPUT_TILT;
	return out;
}
```

- [ ] **Step 2: Enable the chip driver**

Add to `prj.conf`:

```
CONFIG_ALP_SDK_CHIP_BMI323=y
CONFIG_I2C=y
CONFIG_PINCTRL=y
```

- [ ] **Step 3: Feed intents into the loop**

In `src/main.c`, call `tr_imu_open()` after the display opens, and replace `tr_intent_none()` in the step call with `tr_imu_poll()`.

- [ ] **Step 4: Build both targets**

Expected: clean builds. Then bench-run once and record whether tilt actually changes lanes, in the same evidence style as Task 5.

- [ ] **Step 5: Commit**

```bash
git add src/platform/imu.c src/platform/imu.h src/main.c CMakeLists.txt prj.conf
git commit -m "feat(input): add tilt as the secondary control mode"
```

---

### Task 7: The box-to-intent mapping, host-tested

**Files:**
- Create: `src/vision/track.h`, `src/vision/track.c`
- Create: `tests/host/test_track.c`

**Interfaces:**
- Consumes: `tr_intent_t` (Task 1).
- Produces:
  - `typedef struct { int16_t x, y, w, h; uint8_t confidence; bool valid; } tr_box_t;`
  - `typedef struct { int16_t lane_edges[2]; int16_t stance_top; int16_t stance_h; bool calibrated; uint8_t lane; uint8_t lost_frames; } tr_track_t;`
  - `void tr_track_init(tr_track_t *t, int16_t frame_w);`
  - `void tr_track_calibrate(tr_track_t *t, tr_box_t b, int16_t frame_w);`
  - `tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b);`
  - `#define TR_TRACK_MIN_CONF 60`, `#define TR_TRACK_LOST_LIMIT 30`

This is the heart of the primary control scheme and it is **pure logic**, so it is fully host-tested before any camera is involved.

- [ ] **Step 1: Write the failing tests**

```c
/* tests/host/test_track.c */
#include <assert.h>
#include "../../src/vision/track.h"

static tr_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t conf)
{
	return (tr_box_t){ .x = x, .y = y, .w = w, .h = h, .confidence = conf, .valid = true };
}

int main(void)
{
	tr_track_t t;

	/* Calibration: standing centre at 640 wide sets the stance baseline. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	assert(t.calibrated);

	/* Standing still in the centre asks for no lane change. */
	tr_intent_t i = tr_track_update(&t, box(270, 100, 100, 300, 90));
	assert(i.lane_delta == 0 && !i.jump && !i.duck);
	assert(i.source == TR_INPUT_VISION);

	/* Stepping to the player's left moves one lane, once -- not every frame. */
	i = tr_track_update(&t, box(40, 100, 100, 300, 90));
	assert(i.lane_delta == -1);
	i = tr_track_update(&t, box(40, 100, 100, 300, 90));
	assert(i.lane_delta == 0);

	/* Stepping right from there comes back to centre, then right again. */
	i = tr_track_update(&t, box(270, 100, 100, 300, 90));
	assert(i.lane_delta == +1);
	i = tr_track_update(&t, box(500, 100, 100, 300, 90));
	assert(i.lane_delta == +1);

	/* Hysteresis: a small wobble around a band edge must not flip lanes. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	int16_t edge = t.lane_edges[0];
	i            = tr_track_update(&t, box((int16_t)(edge - 52), 100, 100, 300, 90));
	assert(i.lane_delta == -1);
	i = tr_track_update(&t, box((int16_t)(edge - 46), 100, 100, 300, 90));
	assert(i.lane_delta == 0);

	/* A jump: the box top rises sharply against the stance baseline. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	i = tr_track_update(&t, box(270, 40, 100, 300, 90));
	assert(i.jump && !i.duck);

	/* A crouch: the box gets much shorter. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	i = tr_track_update(&t, box(270, 190, 100, 200, 90));
	assert(i.duck && !i.jump);

	/* Low confidence is ignored entirely. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	i = tr_track_update(&t, box(40, 100, 100, 300, 10));
	assert(i.lane_delta == 0 && i.source == TR_INPUT_NONE);

	/* Losing the player for long enough reports it, so the game can pause. */
	tr_track_init(&t, 640);
	tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
	for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
		tr_box_t none = { .valid = false };

		(void)tr_track_update(&t, none);
	}
	assert(tr_track_player_lost(&t));
	return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `./tests/host/runner.sh`
Expected: `BUILD FAIL` — `track.h` does not exist.

- [ ] **Step 3: Write the header**

```c
/* src/vision/track.h */
#ifndef TR_TRACK_H
#define TR_TRACK_H

#include "../game/intent.h"

#define TR_TRACK_MIN_CONF   60 /* Below this the detection is not trusted at all. */
#define TR_TRACK_LOST_LIMIT 30 /* ~1 s at 30 Hz before declaring the player gone. */
#define TR_TRACK_HYST_PX    24 /* Dead band around a lane edge, in frame pixels. */
#define TR_TRACK_JUMP_PX    40 /* Box top must rise this far above stance to count. */
#define TR_TRACK_DUCK_PCT   80 /* Height below this % of stance counts as a crouch. */

typedef struct {
	int16_t x, y, w, h;
	uint8_t confidence;
	bool    valid;
} tr_box_t;

typedef struct {
	int16_t lane_edges[2];
	int16_t stance_top;
	int16_t stance_h;
	bool    calibrated;
	uint8_t lane;
	uint8_t lost_frames;
} tr_track_t;

void        tr_track_init(tr_track_t *t, int16_t frame_w);
void        tr_track_calibrate(tr_track_t *t, tr_box_t b, int16_t frame_w);
tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b);
bool        tr_track_player_lost(const tr_track_t *t);

#endif /* TR_TRACK_H */
```

- [ ] **Step 4: Write the implementation**

```c
/* src/vision/track.c */
#include "track.h"

/*
 * Body tracking, not gesture classification: WHERE the player is beats WHICH of
 * five poses they are striking, both in robustness and in code size.  All three
 * controls fall out of one bounding box.
 *
 * Everything here is pure: no camera, no SDK, no allocation.  That is what lets
 * the whole control scheme be tested on the host before touching hardware.
 */

static uint8_t band_of(const tr_track_t *t, int16_t centre)
{
	/* Hysteresis: the edge you must cross depends on the lane you are in, so a
	 * player standing on a boundary does not flicker between two lanes. */
	int16_t e0 = t->lane_edges[0];
	int16_t e1 = t->lane_edges[1];

	if (t->lane == 0) {
		return (centre > e0 + TR_TRACK_HYST_PX) ? ((centre > e1 + TR_TRACK_HYST_PX) ? 2u : 1u)
						       : 0u;
	}
	if (t->lane == 2u) {
		return (centre < e1 - TR_TRACK_HYST_PX) ? ((centre < e0 - TR_TRACK_HYST_PX) ? 0u : 1u)
						       : 2u;
	}
	if (centre < e0 - TR_TRACK_HYST_PX) {
		return 0u;
	}
	if (centre > e1 + TR_TRACK_HYST_PX) {
		return 2u;
	}
	return 1u;
}

void tr_track_init(tr_track_t *t, int16_t frame_w)
{
	t->lane_edges[0] = (int16_t)(frame_w / 3);
	t->lane_edges[1] = (int16_t)((frame_w * 2) / 3);
	t->stance_top    = 0;
	t->stance_h      = 0;
	t->calibrated    = false;
	t->lane          = 1u;
	t->lost_frames   = 0u;
}

void tr_track_calibrate(tr_track_t *t, tr_box_t b, int16_t frame_w)
{
	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF) {
		return;
	}
	/* The player's own stance is the baseline, so tall and short players, and
	 * players standing at different distances, behave identically. */
	t->stance_top    = b.y;
	t->stance_h      = b.h;
	t->lane_edges[0] = (int16_t)(frame_w / 3);
	t->lane_edges[1] = (int16_t)((frame_w * 2) / 3);
	t->lane          = 1u;
	t->lost_frames   = 0u;
	t->calibrated    = true;
}

bool tr_track_player_lost(const tr_track_t *t)
{
	return t->lost_frames >= TR_TRACK_LOST_LIMIT;
}

tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b)
{
	tr_intent_t out = tr_intent_none();

	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF || !t->calibrated) {
		if (t->lost_frames < 255u) {
			t->lost_frames++;
		}
		return out; /* source stays TR_INPUT_NONE: the caller must not act on this. */
	}
	t->lost_frames = 0u;
	out.source     = TR_INPUT_VISION;

	uint8_t want = band_of(t, (int16_t)(b.x + b.w / 2));

	if (want != t->lane) {
		out.lane_delta = (want > t->lane) ? +1 : -1;
		/* Step one lane per frame so a big sideways stride cannot teleport. */
		t->lane = (uint8_t)(t->lane + (want > t->lane ? 1 : -1));
	}

	if (b.y + TR_TRACK_JUMP_PX < t->stance_top) {
		out.jump = true;
	} else if (t->stance_h > 0 && (b.h * 100) < (t->stance_h * TR_TRACK_DUCK_PCT)) {
		out.duck = true;
	}
	return out;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `./tests/host/runner.sh`
Expected: `PASS` for all three test files.

- [ ] **Step 6: Commit**

```bash
git add src/vision tests/host/test_track.c
git commit -m "feat(vision): map a person bounding box onto lane, jump and duck intents"
```

---

### Task 8: Camera capture and inference on the target

**Files:**
- Create: `src/platform/vision_hw.c`, `src/platform/vision_hw.h`
- Modify: `CMakeLists.txt`, `prj.conf`, `src/main.c`

**Interfaces:**
- Consumes: `tr_box_t` (Task 7).
- Produces:
  - `int tr_vision_open(void);` 0 on success
  - `tr_box_t tr_vision_poll(void);` newest detection, `.valid=false` when none
  - `int16_t tr_vision_frame_width(void);`

Before writing this task's code, read these in the alp-sdk checkout and match their patterns rather than inventing new ones — this task is glue between two bench-verified examples and `track.c`:

- `examples/aen/aen-camera-firstlight` — the MIPI CSI-2 capture path, bench-verified on the AEN EVK and shipped in `b5cce96a1`.
- `chips/ov9281/ov9281.c` and `include/alp/chips/ov9281.h` — the sensor driver and its public API.
- `examples/aen/aen-npu-inference-person-mram` — loading a model from MRAM and invoking it.

Camera traps already paid for once, so do not rediscover them: `ALP_ERR_IO` on open means the video buffer was 4-aligned when the CPI needs 8; `ALP_ERR_NOMEM` means `SYS_HEAP_SMALL_ONLY` capped the pool at 2 MiB; a capture that succeeds but looks like noise means `CONFIG_VIDEO_ALIF_CAM_EXTENDED` is off, so `AXI_PORT_EN` is never set.

- [ ] **Step 1: Write the capture + inference layer**

```c
/* src/platform/vision_hw.h */
#ifndef TR_PLATFORM_VISION_HW_H
#define TR_PLATFORM_VISION_HW_H

#include "../vision/track.h"

int      tr_vision_open(void);
tr_box_t tr_vision_poll(void);
int16_t  tr_vision_frame_width(void);

#endif /* TR_PLATFORM_VISION_HW_H */
```

```c
/* src/platform/vision_hw.c */
#include <alp/inference.h>
#include <alp/peripheral.h>
#include <zephyr/sys/printk.h>

#include "vision_hw.h"

/*
 * Camera frame -> person detector -> one bounding box.  The box is all the game
 * ever sees of the camera; everything downstream is pure logic in vision/track.c.
 *
 * The model lives in MRAM, which is why this milestone moves the project off the
 * 256 KB ITCM RAM-run and onto the MRAM slot0 flash flow.
 */

extern const uint8_t  tr_person_model[];  /* Linked in from the model blob. */
extern const uint32_t tr_person_model_len;

static alp_inference_t *g_inf;
static int16_t          g_frame_w;

int tr_vision_open(void)
{
	alp_inference_config_t cfg = {
		.model_data  = tr_person_model,
		.model_size  = tr_person_model_len,
		.format      = ALP_INFERENCE_MODEL_TFLITE,
		.backend     = ALP_INFERENCE_BACKEND_AUTO,
		.arena_bytes = 0, /* Let the backend size its own arena. */
		.arena       = NULL,
	};

	g_inf = alp_inference_open(&cfg);
	if (g_inf == NULL) {
		printk("vision  : inference open failed -- body tracking unavailable\n");
		return -1;
	}

	alp_inference_tensor_t in;

	if (alp_inference_get_input(g_inf, 0, &in) != ALP_OK) {
		printk("vision  : input tensor unavailable\n");
		return -1;
	}
	/* Shape is most-significant first: [batch, height, width, channels]. */
	g_frame_w = (int16_t)in.shape[2];
	printk("vision  : model input %ux%u, frame width %d\n", in.shape[2], in.shape[1], g_frame_w);
	return 0;
}

int16_t tr_vision_frame_width(void)
{
	return g_frame_w;
}

tr_box_t tr_vision_poll(void)
{
	tr_box_t out = { .valid = false };

	if (g_inf == NULL) {
		return out;
	}

	alp_inference_tensor_t in;

	if (alp_inference_get_input(g_inf, 0, &in) != ALP_OK) {
		return out;
	}
	/* Capture straight into the model's own input buffer: one fewer copy per
	 * frame, and the camera example already hands back a buffer we can convert. */
	if (tr_camera_fill(in.data, in.size_bytes) != 0) {
		return out;
	}
	if (alp_inference_invoke(g_inf) != ALP_OK) {
		return out;
	}

	alp_inference_tensor_t det;

	if (alp_inference_get_output(g_inf, 0, &det) != ALP_OK) {
		return out;
	}
	return tr_decode_box(&det); /* Model-specific; see Step 2. */
}
```

- [ ] **Step 2: Write the two model-specific helpers**

`tr_camera_fill()` and `tr_decode_box()` depend on the exact camera format and model output, which must be read from the two SDK examples rather than assumed. Implement both in the same file, directly above `tr_vision_poll()`, and write a comment recording:
- the camera format the SDK negotiated (from the `aen-camera-regcheck` output),
- the model's output tensor shape and what each element means,
- the scale and zero point if the output is quantised.

If the model's output cannot be decoded into a single box with a confidence, stop and report that: it is a finding against the model choice, not something to paper over.

- [ ] **Step 3: Switch to the MRAM flash flow**

The image plus model exceeds the 256 KB ITCM RAM-run. Use the MRAM slot0 flash flow for this and every later milestone, and note the change in `README.md` so nobody tries a RAM-run and wonders why it truncates.

- [ ] **Step 4: Prove the model runs, before it controls anything**

Draw the detected box on screen as an outline and print one line per second with its coordinates and confidence. Do **not** wire it to the game yet. Bench-run it and judge the tracking by eye: stand, step left and right, jump, crouch.

Record the result in `docs/bench/`, with a camera frame.

- [ ] **Step 5: Commit**

```bash
git add src/platform/vision_hw.c src/platform/vision_hw.h CMakeLists.txt prj.conf src/main.c docs/bench
git commit -m "feat(vision): run person detection on the target and show the box on screen"
```

---

### Task 9: Body tracking takes the controls

**Files:**
- Modify: `src/main.c`
- Create: `src/game/mode.h`

**Interfaces:**
- Consumes: `tr_vision_*` (Task 8), `tr_track_*` (Task 7), `tr_imu_*` (Task 6).
- Produces: `typedef enum { TR_MODE_VISION, TR_MODE_TILT } tr_mode_t;`

- [ ] **Step 1: Add mode selection and calibration**

```c
/* src/game/mode.h */
#ifndef TR_MODE_H
#define TR_MODE_H

/*
 * The two control schemes are EXCLUSIVE and chosen before the run starts.
 * Swapping scheme under a player mid-run feels broken, so losing the player
 * pauses the game instead of silently handing control to tilt.
 */
typedef enum {
	TR_MODE_VISION = 0, /**< Stand in front of the board and move. */
	TR_MODE_TILT,       /**< Hold the board and tilt it. */
} tr_mode_t;

#endif /* TR_MODE_H */
```

In `main()`: if `tr_vision_open()` succeeds, use `TR_MODE_VISION`; otherwise fall back to `TR_MODE_TILT` and say so on the console. Before the first run in vision mode, show a "stand in the middle" title screen for 2 seconds, then call `tr_track_calibrate()` with the newest box.

- [ ] **Step 2: Feed the right intent into the tick**

```c
	tr_intent_t in = tr_intent_none();

	if (mode == TR_MODE_VISION) {
		tr_box_t b = tr_vision_poll();

		in = tr_track_update(&track, b);
		if (tr_track_player_lost(&track)) {
			/* Pause rather than continue blind: the run is the player's, and
			 * a runner that keeps going while nobody is there just dies. */
			paused = true;
		} else if (in.source == TR_INPUT_VISION) {
			paused = false;
		}
	} else {
		in = tr_imu_poll();
	}
	if (!paused) {
		tr_game_step(&g, in, (int16_t)tr_display_height());
	}
```

- [ ] **Step 3: Show the paused state on screen**

Draw a clear "step back into view" banner while paused, so a player who wandered out of frame understands why the game stopped.

- [ ] **Step 4: Build both targets, then bench-run**

Bench-run on `a bench E1M-AEN803`, cold cycle first. The evidence for this milestone is a video or frame sequence showing a person stepping left and right, jumping and crouching, with the runner following. Record it in `docs/bench/`.

- [ ] **Step 5: Commit**

```bash
git add src/main.c src/game/mode.h docs/bench
git commit -m "feat: let body tracking drive the game, with tilt as the alternative mode"
```

---

### Task 10: Camera preview on display layer 2

**Files:**
- Modify: the shield overlay in the alp-sdk checkout (a separate PR against alp-sdk), `src/platform/display.c`

**Interfaces:**
- Consumes: the camera frame from Task 8.
- Produces: `int tr_display_preview(const void *px, uint16_t w, uint16_t h);`

- [ ] **Step 1: Enable L2 in the shield**

The CDC200 binding supports `enable-l2` and `pixel-fmt-l2` with its own framebuffer. Enabling it is an **alp-sdk change**, so it goes in an alp-sdk branch and PR — not vendored here. The overlay must place the L2 framebuffer in SRAM1 and must not overlap `sram0`, `lcd_fb` or the linker region.

- [ ] **Step 2: Write the preview path**

Blit the camera frame to L2 through the display API. The game keeps drawing to L1 exactly as before, and the CDC blends them, so the preview costs the game loop nothing.

- [ ] **Step 3: Verify the composition on silicon**

Bench-run and photograph the glass: the game must be legible over a dimmed live camera image, and the frame rate must not drop from the M0 measurement. If it does, the preview is costing CPU somewhere and the cause must be found before this task closes.

- [ ] **Step 4: Commit**

```bash
git add src/platform/display.c docs/bench
git commit -m "feat(display): show the live camera preview on layer 2 behind the game"
```

---

### Task 11: Art pipeline and sprite rendering

**Files:**
- Create: `art/` (one source PNG per sprite, RGBA)
- Create: `tools/mkatlas.py`
- Create: `src/render/atlas.h`, `src/render/sprite.c`
- Create: `tests/host/test_atlas.c`
- Modify: `src/render/render.c`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `tr_display_blit` (Task 3).
- Produces:
  - `typedef struct { uint16_t w, h; const uint16_t *px; const uint8_t *mask; } tr_sprite_t;`
  - `static inline bool tr_sprite_opaque(const tr_sprite_t *s, uint16_t x, uint16_t y);`
  - `void tr_sprite_draw(int16_t x, int16_t y, const tr_sprite_t *s, uint16_t bg);`
  - `extern const tr_sprite_t tr_spr_runner_run[4], tr_spr_runner_jump, tr_spr_runner_duck, tr_spr_obstacle_low, tr_spr_obstacle_high, tr_spr_pickup[4];`

The demo is judged on how it looks, so this is not polish — it is what makes the thing worth filming.

**Art direction:** a circuit-board world seen from above. Copper traces as lanes over dark solder mask, components as obstacles, bright solder blobs as pickups, and a runner character designed for this project. Every asset is original to this project; nothing is derived from any existing game, character or franchise.

**Quality bar:** a 4-frame run cycle, silhouettes readable at arm's length, a limited palette chosen to survive RGB565 banding, and edges anti-aliased against the background colour.

- [ ] **Step 1: Author the source art**

One PNG per sprite in `art/`, RGBA, at final pixel size: runner 96x96, obstacles 88x88, pickups 48x48 — large enough to look deliberate on a 720x1280 panel. Use whatever tool gives the best result, including an image model, as long as every asset is original to this project.

- [ ] **Step 2: Write the converter**

`tools/mkatlas.py` reads `art/*.png` and emits one C file holding, per sprite, an RGB565 pixel array and a 1-bit alpha mask, plus a `tr_sprite_t` for each. RGB565 because that is the panel's format; a separate mask because the layer this draws into has no per-pixel alpha, so the blitter needs to know which pixels to skip rather than punching background-coloured holes.

```python
#!/usr/bin/env python3
# Convert art/*.png into a C sprite atlas: RGB565 pixels plus a 1-bit alpha mask.
import pathlib
import sys

from PIL import Image


def to565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def emit(name, img):
    img = img.convert("RGBA")
    w, h = img.size
    px, mask, bits, nbits = [], [], 0, 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = img.getpixel((x, y))
            px.append(to565(r, g, b))
            bits = (bits << 1) | (1 if a >= 128 else 0)
            nbits += 1
            if nbits == 8:
                mask.append(bits)
                bits = nbits = 0
    if nbits:
        mask.append(bits << (8 - nbits))
    lines = [f"/* Generated from art/{name}.png by tools/mkatlas.py -- do not edit. */"]
    lines.append(f"static const uint16_t {name}_px[] = {{")
    lines.append(",".join(f"0x{v:04x}" for v in px))
    lines.append("};")
    lines.append(f"static const uint8_t {name}_mask[] = {{")
    lines.append(",".join(f"0x{v:02x}" for v in mask))
    lines.append("};")
    lines.append(
        f"const tr_sprite_t tr_spr_{name} = {{ .w = {w}, .h = {h},"
        f" .px = {name}_px, .mask = {name}_mask }};"
    )
    return "\n".join(lines)


def main(argv):
    art, dst = pathlib.Path(argv[1]), pathlib.Path(argv[2])
    pngs = sorted(art.glob("*.png"))
    chunks = ['#include "atlas.h"', ""]
    chunks += [emit(p.stem, Image.open(p)) for p in pngs]
    dst.write_text("\n".join(chunks) + "\n")
    print(f"wrote {dst} from {len(pngs)} sprite(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
```

- [ ] **Step 3: Write the failing test for mask bit order**

```c
/* tests/host/test_atlas.c
 *
 * The mask is read MSB-first.  Get this backwards and every sprite comes out
 * mirrored in 8-pixel blocks, which is maddening to diagnose on glass.
 */
#include <assert.h>
#include "../../src/render/atlas.h"

int main(void)
{
	/* 8x1 sprite, alpha 1,0,1,0,1,0,1,0 -> mask byte 0xAA. */
	static const uint16_t px[]   = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const uint8_t  mask[] = { 0xAAu };
	tr_sprite_t           s      = { .w = 8, .h = 1, .px = px, .mask = mask };

	assert(tr_sprite_opaque(&s, 0, 0));
	assert(!tr_sprite_opaque(&s, 1, 0));
	assert(tr_sprite_opaque(&s, 6, 0));
	assert(!tr_sprite_opaque(&s, 7, 0));
	return 0;
}
```

- [ ] **Step 4: Run to verify it fails**

Run: `./tests/host/runner.sh`
Expected: `BUILD FAIL` — `atlas.h` does not exist.

- [ ] **Step 5: Write the header and blitter**

```c
/* src/render/atlas.h */
#ifndef TR_ATLAS_H
#define TR_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint16_t        w, h;
	const uint16_t *px;
	const uint8_t  *mask; /**< 1 bit per pixel, row-major, MSB first. */
} tr_sprite_t;

static inline bool tr_sprite_opaque(const tr_sprite_t *s, uint16_t x, uint16_t y)
{
	uint32_t bit = (uint32_t)y * s->w + x;

	return (s->mask[bit >> 3] >> (7u - (bit & 7u))) & 1u;
}

/* Composes the sprite over `bg` in a scratch buffer, then issues ONE blit --
 * never one blit per pixel, which would swamp the display path. */
void tr_sprite_draw(int16_t x, int16_t y, const tr_sprite_t *s, uint16_t bg);

#endif /* TR_ATLAS_H */
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `./tests/host/runner.sh`
Expected: `PASS` for all four test files.

- [ ] **Step 7: Replace the placeholder rectangles**

In `render.c`, swap each `paint(...)` call for `tr_sprite_draw(...)`, keeping the erase-all / move-all / draw-all order exactly as it is. Animate the runner by indexing `tr_spr_runner_run[]` off the tick counter, and switch to the jump and duck sprites from game state.

- [ ] **Step 8: Build, bench-run, and judge it by eye**

The evidence for this task is a photograph of the glass. If it does not look good at arm's length, the task is not finished.

- [ ] **Step 9: Commit**

```bash
git add art tools/mkatlas.py src/render tests/host/test_atlas.c CMakeLists.txt
git commit -m "feat(render): replace placeholder rectangles with original sprite artwork"
```

---

## What comes after this plan

M4-M8 get their own plans once the core is real on silicon: the HP/HE core split with the mailbox, the GPU2D sprite path, NOR asset streaming, touch menus once the GT911 config is solved, and audio plus a BLE two-board race.

The gaps this project is expected to file against the SDK or the SoM roadmap, rather than work around: the A32 Linux display, camera and NPU stack; the Yocto machine build failures; the GT911 blank config; the HyperRAM CK/CK# crossing; the intermittent panel init; and OSPI device-level access.
