### Fixed — `openocd-ram-run.sh` reset-catches the core before loading (#2422)

The intermittent BusFault in `aen-cc3501e-socket-throughput` (#2422) was a bench
artefact, not an SDK bug. When the pre-load `halt` caught the resident app
inside an interrupt or PendSV handler, that exception stayed **active** through
`load_image` and `reg xPSR`. The freshly loaded image then ran its whole life in
Handler mode and, once `main()` returned, faulted in interrupt context on stale
data the resident had left in the uninitialised stacks.

Measured on E1M-AEN803 serial 2026W36-0009:
- **Resident caught in its IRQ 333 handler:** 6 of 6 runs faulted. LR was
  `z_thread_halt`, matching both original captures, and PC was a stale pixel
  word.
- **Resident caught in PendSV:** 3 of 3 runs faulted in `remove_timeout`.
- **With a reset-catch before the load:** 0 of 3 runs faulted.
- **Ruled out:** forced "PING never answered" over a clean core (0 of 10), the
  idle path after `main()` returns, and `CONFIG_DCACHE=n` (0 of 10).

The script now sets DEMCR `TRCENA|VC_CORERESET` and writes AIRCR `SYSRESETREQ`
after the halt. The core stops at its reset vector in Thread mode with every
exception inactive before the image is loaded. The J-Link `ram-run.sh` already
gets this from `loadbin`'s implicit reset. Verified on the M55-HE; `core=hp`
uses the same sequence.
