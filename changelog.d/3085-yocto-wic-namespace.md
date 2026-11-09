### Fixed -- yocto_wic refuses a busy target in any mount namespace (#3085)

**The in-use check no longer depends on the caller's mount table.** `_require_unused_device` now also refuses a target (or a partition of it) whose `O_RDONLY|O_EXCL` open fails with `EBUSY`, or whose sysfs `holders` is non-empty, so a container with the host's `/dev/sdX` passed through can no longer overwrite the host root disk; the target is identified by `st_rdev` rather than by name (mknod aliases match), and an unreadable `/proc/swaps` or any other "cannot tell" errno refuses instead of proceeding.
