### Fixed — tests: fuzz production mproc decoder, make HIL ssh-run exit codes count (#2976, #2996)

The mproc_frame fuzz harness fuzzed an inline copy of the decoder that had
drifted from src/common/proto/alp_mproc_frame.c (it rejected a NULL sequence
output the Zephyr driver passes, and returned its own status codes). It now
links the production framing and checks NULL-output parity, zeroed outputs on
rejection and an encode/decode round trip with the production encoder.

run_smoke.py discarded the exit code of ssh-run binaries and ssh_command
one-liners, so a crash after the expected banner still passed. The remote
wrapper now exits with the example's status; a non-zero status fails the
spec, except 143 (SIGTERM from the bounded-run sleeper stopping a
forever-looping example).
