### Fixed — examples: register mproc-mailbox callback before send, bound peer reply, fail rpmsg-aen on missing samples (#2971, #2994)

the mproc-mailbox example now registers its mailbox receive callback before sending the request, so a fast peer reply is no longer dropped and reported as a timeout, and it bounds-checks the peer-supplied reply offset and length against the shared-memory size before reading. The rpmsg-aen Linux consumer now exits 1 when fewer than the expected 4 samples arrive instead of reporting success.
