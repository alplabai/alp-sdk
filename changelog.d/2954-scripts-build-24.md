### Fixed — orchestrate: warn on blocked IPC carve-outs, refuse unresolved Alif aperture, emit OTA west module, MCP emit core/build-root (#2954, #2988)

A blocked IPC carve-out emitted zero-stub macros and no plan warning, so
every slice built green and the defect only showed at runtime. The build
plan now carries a blocked-ipc-carveout warning per endpoint core; the
stale comments claiming an #error directive are corrected.

carveout.py fell back to the legacy `carveout: false` filter when no MRAM
aperture resolved, making mram_main (ATOC band included) a valid IPC
target on an Alif SoM. An Alif preset with an unresolved aperture is now
refused with a reason.

west-libraries never saw `ota:` because _v1_shaped_project dropped it, so
mender-mcu-client was never emitted. The key is now carried through.

The MCP emit tool accepts core and build_root (kconfig needs core) and
runs with the project directory as cwd.

Auto-placed storage offsets move when a sibling is added or renamed; the
docstring and docs now say shipped products must pin offset_kib, with a
test that pinned offsets stay fixed.
