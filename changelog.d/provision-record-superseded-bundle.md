### Fixed — provision `record` keeps bundle facts across a tool_rev-only supersession

A unit flashed from a signed bundle lost its bundle facts (`rootfs_bundle_version`, `bl2_sha256`, ...) once the tool's git HEAD moved, because `run` moves the flash steps to `superseded` and `record` only trusted the current state, leaving `ship_check` blocked on an unsigned `--build-dir`. `record` now counts a flash step finished in a superseded run whose `bundle_sha256` equals the current bundle's, and notes that provenance in the unit's `.md`; a supersession by a different bundle still does not count.
