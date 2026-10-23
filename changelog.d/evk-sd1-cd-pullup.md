### Fixed — E1M-X EVK microSD card-detect over-voltage on PA1

- `e1m-x-evk.dtsi`: `&sdhi1` `cd-gpios` now requests the SoC's internal pull-up (`GPIO_PULL_UP`). On the E1M-X EVK V2 the card-detect pull-up `R53` goes to the 3.3 V card rail, while `PA1` is a 1.8 V pad on the V2N SoMs, so an empty slot over-drives it. With the internal pull-up in place, `R53` can be removed as a board rework. `docs/boards/e1m-x-evk.md` describes it.
