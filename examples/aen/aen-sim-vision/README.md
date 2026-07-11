# aen-sim-vision — camera → inference → display, in the Renode sim

A minimal vision pipeline that runs **real TensorFlow-Lite-Micro inference
on the Alif Ensemble E8 Cortex-M55-HP**, driven by the `west alp-renode
--sim-mode` studio hardware simulator (alp-sdk#687). No NPU is used — the
inference is the **software path** (TFLM on the M55, Helium-accelerated),
which is exactly what a sim validates: the pipeline + the *result*, not the
Ethos-U hardware dispatch (a bench/HIL concern).

## Loop
1. Studio injects a "camera frame" into `SIM_FRAME` (0x20041000) — the
   E1M-AEN801 descriptor's camera `inject.memcpy` base — then fires its
   trigger (rings `SIM_DOORBELL`, 0x20042000).
2. The app derives a feature from the frame, runs the TFLM model, and
   renders the result into the ssd1306 MONO frame buffer (`SIM_DISPLAY`,
   0x20040000 — the descriptor's `framebuffers[]` base).
3. It logs an `inference:` line to the UART5 console.

Studio reads the frame buffer + console back over the sim sockets.

## Build + run
Build **ITCM-linked** (Renode 1.16.1's Cortex-M55 mis-executes a high-MRAM
vector table; ITCM@0x0 sidesteps it):

    west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
        examples/aen/aen-sim-vision \
        -- -DEXTRA_DTC_OVERLAY_FILE=$PWD/tests/renode/aen_m55_itcm_run.overlay
    west alp-renode --sim-mode --board E1M-AEN801 --image-bundle build

The bundled (sine) model is a placeholder — swap `src/model.cpp` for a real
vision net and the pipeline is unchanged.
