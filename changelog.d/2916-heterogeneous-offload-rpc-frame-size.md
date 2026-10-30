### Fixed — size heterogeneous-offload FFT RPC frames to fit the transport cap (#2916)

The example documented a 1024-point FFT: a 4096 B request and a 2052 B
response. That exceeds ALP_RPC_TX_FRAME_MAX / CONFIG_ALP_SDK_RPC_TX_FRAME_MAX
(1024) on both sides, and a single RPMsg buffer (496 B usable), so every
alp_rpc_call("fft") failed on silicon. Reduce the transform to 64 points
(256 B request, 132 B response) on both the A55 and M33-SM sides, retune the
synthetic tones to 400/800 Hz at 6400 Hz so they land on exact bins, and
document the frame budget in the teaching comments and README. The requester
now returns non-zero when an fft call fails instead of reporting success.
