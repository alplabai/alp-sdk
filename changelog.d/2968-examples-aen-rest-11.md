### Fixed — examples/aen: make validation gates real and stop sticky/glitching bring-up state (#2968, #2969, #2970, #2993)

- aen-analog-validate: PASS now requires the DAC readback and ADC read to
  succeed and the loopback to land within tolerance; out-of-tolerance prints
  RESULT FAIL.
- aen-cc3501e-bringup: latch nRESET (P15_1) high in the LP-GPIO DR before
  switching it to output when it reads high, so a warm re-run does not hold a
  running chip in reset; the bridge helper copies are re-synced.
- aen-cc3501e-bringup: the OTA post-failure probe uses cc3501e_sync()
  (host-only SPI resync) instead of cc3501e_reset(), which cycled nRESET and
  WIFI_EN mid-OTA.
- aen-dualcore-ipc: HP serving some round-trips and then going silent is a
  FAIL, SKIP stays for the never-saw-the-peer case.
- aen-eeprom-provision: the provisioning safety flags are consumed per
  configure and removed from the CMake cache; test added.
- aen-npu-inference-alp: zero the output tensor before invoke so the
  non-zero PASS check proves the NPU wrote it.
