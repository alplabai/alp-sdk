### Fixed — warm-aware CC3501E bring-up, return error when chip is silent (#2918)

The alp-console cc3501e_bridge.c carried a stale bring-up: it called
cc3501e_reset() unconditionally, dropping WIFI_EN for 50 ms on every
re-run (an activated unit with vendor_sbl_container_enable=1 may never
relaunch), configured WIFI_EN as an output before latching DR high
(glitching a live supply), and returned ALP_OK after the GET_VERSION
soak even when the chip never answered, so the console registered a dead
companion as healthy.

Align with the maintained aen-cc3501e-* bring-up: sample WIFI_EN, latch
DR high before the direction change, nRESET-only when WIFI_EN is high
and PING answers, supply cycle only from WIFI_EN low or a silent chip,
and propagate the resulting status. Comments and README updated.
