### Fixed — the TAS2563 bring-up no longer hits the amp's NACK window after `SD_N` (standalone `sound/` too)

On the bench carrier U27 (`0x4D`) NACKs its address 313 us after `SD_N` rises and first ACKs at 1142 us, and the SDK's 200 us settle landed in that window (`tas2563_init(0x4d)` returned -5). `sound/src/main.c` now waits 2 ms, then polls each amp for its address ACK every 1 ms, at most 50 ms; an amp that never answers fails the bring-up at step 6. It applies to the standalone GAME and TEST images and to the combined image.
