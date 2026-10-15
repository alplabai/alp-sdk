### Added — CSI-2 error events counted per cause (#2880)

`zephyr/drivers/video/video_csi_dw.c` counts every CSI-2 error interrupt by cause in `csi2_dw_err_events[]` (payload CRC, frame CRC, corrected header ECC, data ID, line, PHY, packet, PHY fatal, frame sequence, frame boundary, IPI fatal), since boot and whether or not logging is on. Before, the driver only logged them, so a bright frame with hundreds of payload-CRC events could not be counted. Read the array with `extern volatile uint32_t csi2_dw_err_events[];` or over SWD; the difference across N frames gives the per-frame rate.
