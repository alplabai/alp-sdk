.. _e1m_evk_microsd_shield:

E1M-EVK microSD slot (J7)
#########################

Overview
********

J7 on the E1M-EVK is a microSD slot wired 4-bit to the SoM's SD host through
the carrier's SDIO mux (U38/U39). This shield enables that host and declares
the card as the Zephyr disk ``SD``, so an application can call
``disk_access_init("SD")`` or mount a FatFs volume on ``/SD:``.

The mux select is strapped by header P18 (open = microSD). The mux enable
``/E`` is E1M ``IO20``, active low. On the E1M-AEN SoMs ``IO20`` is driven by
the on-module CC3501E, so the application brings the CC3501E bridge up and
drives ``IO20`` low before it first touches the disk.

Requirements
************

- An E1M-EVK 2626-R2 whose U38/U39 are the reworked 74LV3257 bus switches.
  The stock 74LVC157 parts contend with the SoC's SD pins while enabled.
- Header P18 open.
- A SoM target this shield has a ``boards/`` overlay for: E1M-AEN401 (M55-HP),
  E1M-AEN801 and E1M-AEN803 (M55-HE and M55-HP).

The SD host's DMA engine cannot reach the M55 TCMs, so the driver copies
through an 8 KB buffer in SRAM0 (``CONFIG_SDHC_DWC_BOUNCE_BUFFER``). Each
core's image places its SRAM0 sections from ``0x02000000``: when the other
M55 also uses SRAM0 (NPU arena, video buffers), give one of them its own
window. Do not set ``chosen { zephyr,sram = &sram0; }`` with this shield; the
build refuses it.

Programming
***********

.. code-block:: console

   west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he <app> -- -DSHIELD=e1m_evk_microsd

See ``examples/aen/aen-sdcard-file`` for the complete sequence.
