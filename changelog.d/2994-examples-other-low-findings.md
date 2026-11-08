### Fixed — `microros-ros2-v2n` M33 doorbell follows the shared header selection (#2994)

**The M33 firmware now rings the doorbell line chosen by `<alp/protocol/v2n_mhu_doorbell.h>` instead of a hard-coded `0x504808C4`.** Building with `CONFIG_ALP_V2N_DOORBELL_RSP_CH8` previously left this firmware on SWINT unit 12 while the A55 side listened on GIC_SPI 385, so every M33-to-A55 XRCE reply was lost. The default build resolves to the same address as before. Not built (needs the micro-ROS colcon toolchain) and not bench-verified.
