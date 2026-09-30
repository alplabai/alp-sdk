### Added — PDM capture at 8 and 16 kHz on boards whose mics can't be clocked for it (#2134)

`alp_audio_in_open(.sample_rate_hz = 16000)`, and 8000, used to fail with
`ALP_ERR_INVAL` on the E1M-EVK. Its MP34DT05TR-A mics need a PDM clock of
at least 1.2 MHz, and the Alif modes for 8 and 16 kHz run slower than that.

The Zephyr audio-in backend now tries the requested rate first. If the PDM
refuses it (`-EINVAL`), the backend opens at the lowest native rate that is
an integer multiple of the request: 32 kHz, then 48 kHz. It then decimates
each block with the `<alp/dsp.h>` decimator from phase 1 (#2160).
- Reads still return frames at the requested rate.
- `frames_per_block` still counts frames at that rate.
- Only S16 is supported.

The decimator moved out of `src/dsp_dispatch.c` into `src/dsp_decimator.c`.
PDM capture now links it without pulling in the DSP chain's handle pool;
`gen_dsp_decimator_coeffs.py` and its test follow the move.

Bench-verified on E1M-AEN803 2026W36-0009 (Flow C, M55-HE, one EVK PDM
mic), each from a cold boot:
- 16000 Hz: `ALP_OK`, 16042 Hz delivered (32 kHz native, ratio 2), live
  signal. On unmodified `dev` the same open returns `ALP_ERR_INVAL`.
- 8000 Hz: 8021 Hz delivered (32 kHz native, ratio 4).
- 48000 Hz: native and unchanged, 48139 Hz delivered.
