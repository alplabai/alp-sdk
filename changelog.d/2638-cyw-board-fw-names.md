### Fixed — board-named brcmfmac firmware ships as symlinks (#2638)

The Wi-Fi firmware recipe links `cyfmac55500-sdio.<board>.{trxse,txt,clm_blob}` to the generic files for each board DT compatible, so the first firmware lookup succeeds. Built into the `e1m-v2m103-a55` image (container build); not run on a board.
