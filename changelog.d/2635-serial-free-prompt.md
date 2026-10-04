### Changed — root shell prompt no longer shows the unit serial; the login banner does (#2635)

Login shells show `root@e1m-v2m103:~#` while the hostname keeps the serial; the pre-login banner gains a `Module: <SKU>  Serial: <serial>` line (written to `/run/alp-module.issue`, linked from `/etc/issue.d/10-alp-module.issue`). Built into the `e1m-v2m103-a55` image (container build); not run on a board.
