### Changed — aen-cc3501e-socket-throughput sweeps STREAM_WRITE to the 4092-byte ceiling (#1677)

The bridge-only sweep used to stop at 512 B. An earlier bench, run on
mismatched host/firmware protocol revisions, had failed 1024 B and 4092 B
with `rc=-5` and a link that never recovered. Re-measured on 2026-09-28 on
E1M-AEN803 2026W36-0009, with this tree's host and a CC3501E on the bridge
firmware's main branch (`GET_VERSION` v1024): 512, 1024, 2048 and 4092 B
each pushed the full 1 MiB window, in 3 of 3 runs, with no `rc=-5` and no
wedge. The sweep now covers 64 to 4092 B. Rates: 44947 B/s at 64 B,
185654 B/s at 512 B, 226034 B/s at 1024 B, 268893 B/s at 4092 B. The send
buffer is now sized for the largest entry: it was 512 B, and the loop
refuses anything larger than the buffer.
