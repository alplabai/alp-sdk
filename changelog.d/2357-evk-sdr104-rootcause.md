### Documentation — E1M-X EVK microSD: SDR104 write failure root cause, mux enable

- `docs/boards/e1m-x-evk.md`: at SDR104 200 MHz the SDHI host mis-frames the card's write CRC-status token on DAT0 (`SD_ERR_STS1` reports `100`/`001`/`101` instead of `010`). The data does reach the card. Only one SCC sampling tap frames the token correctly, and it is not the tuned read tap, so `&sdhi1` stays capped at SDR50. The same page now explains that with GD32 bridge firmware running, `IO29` (`SD_MUX_EN`) must be driven low or the slot reports no card, while card detect still reads present.
