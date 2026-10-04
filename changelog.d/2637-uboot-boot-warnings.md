### Fixed — the stale vendor dtb name no longer warns on every U-Boot boot (#2637)

The image links the stock `boot/r9a09g056n44-dev.dtb` name to the board dtb so the vendor load succeeds; the ALP boot command still loads the real board dtb afterwards. The image build now fails when the board dtb is missing from the rootfs. The `alp-image-base` image for `e1m-v2m103-a55` was built in a container and holds the link; not run on a board.
