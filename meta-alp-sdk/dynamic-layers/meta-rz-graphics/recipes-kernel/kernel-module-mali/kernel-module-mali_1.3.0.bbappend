# meta-alp-sdk: GPU clock-ownership fix for the Renesas kernel-module-mali
# recipe (Mali-G31 DDK r54, kbase r54p1) on the E1M RZ/V2N/V2M SoMs.
#
# The CPG genpd manages the GPU module clocks via pm_clk (GENPD_FLAG_PM_CLK):
# it prepares them at attach and enables/disables them across runtime
# resume/suspend.  The kbase devicetree platform ALSO managed the same clocks
# in enable/disable_gpu_power_control(), double-counting the prepare refcount.
# On suspend kbase's clk_disable_unprepare() dropped the prepare pm_clk relies
# on, so the next pm_clk_resume() clk_enable() warned "Enabling unprepared
# gpu_0_clk" -> -ESHUTDOWN on every compositor repaint (~60 s), and the GPU
# could not power back on.  Patch 0005 skips the clock loops when the device
# sits in a PM domain so genpd owns the clocks (kbase keeps the regulator and
# resets).  Silicon-validated 2026-06-11 on E1M-V2M101: zero unprepared-clk
# warnings across multiple suspend/resume cycles; the GPU clocks stay prepared
# and enabled.
#
# Scoped to the 1.3.0 (DDK r54) recipe because the patch context tracks that
# DDK version -- re-verify it when the kernel-module-mali PV bumps.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://0005-platform-devicetree-leave-GPU-clocks-to-the-PM-domai.patch;striplevel=5"
