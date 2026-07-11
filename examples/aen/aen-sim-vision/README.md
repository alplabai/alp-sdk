# aen-sim-vision — camera → inference → display + mic → wake-word, in the Renode sim

Two minimal pipelines in one image, both running **real TensorFlow-Lite-Micro
inference on the Alif Ensemble E8 Cortex-M55-HP**, driven by the `west
alp-renode --sim-mode` studio hardware simulator (alp-sdk#687). No NPU is
used — the inference is the **software path** (TFLM on the M55,
Helium-accelerated), which is exactly what a sim validates: the pipeline +
the *result*, not the Ethos-U hardware dispatch (a bench/HIL concern).

## Vision loop
1. Studio injects a "camera frame" into `SIM_FRAME` (0x20041000) — the
   E1M-AEN801 descriptor's camera `inject.memcpy` base — then fires its
   trigger (rings `SIM_DOORBELL`, 0x20042000). The descriptor advertises the
   expected frame encoding (`GRAY8` 32×32 = 1024 B) so studio resizes the
   user's image to match before the memcpy.
2. The app derives a feature from the frame, runs the TFLM model, and
   renders the result into the ssd1306 MONO frame buffer (`SIM_DISPLAY`,
   0x20040000 — the descriptor's `framebuffers[]` base).
3. It logs an `inference:` line to the UART5 console.

## Wake-word loop
1. Studio injects an "audio clip" into `SIM_AUDIO` (0x20043000) — the
   descriptor's mic `inject.memcpy` base — then fires its trigger (rings
   `SIM_AUDIO_DOORBELL`, 0x20044000).
2. The app derives a loudness feature, runs the same TFLM model for a
   confidence score, and gates a wake word on the feature.
3. It logs a `wakeword: detected|absent …` line to the UART5 console.

The placeholder sine model can't classify, so the loudness gate stands in as
the detector; a real KWS net (swap `src/model.cpp`) makes the model output
the classifier score.

Studio reads the frame buffer + console back over the sim sockets.

## Build + run
Build **ITCM-linked** (Renode 1.16.1's Cortex-M55 mis-executes a high-MRAM
vector table; ITCM@0x0 sidesteps it):

    west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
        examples/aen/aen-sim-vision \
        -- -DEXTRA_DTC_OVERLAY_FILE=$PWD/tests/renode/aen_m55_itcm_run.overlay
    west alp-renode --sim-mode --board E1M-AEN801 --image-bundle build

The bundled (sine) model is a placeholder — swap `src/model.cpp` for a real
vision or KWS net and the pipelines are unchanged.
