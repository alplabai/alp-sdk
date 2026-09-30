### Added

- `tests/hil/v2m103-x-evk/v2m103-can-two-node.yaml`: opt-in HIL spec for the two on-module CAN-FD ports cabled together (#2332). Sends classic, FD and FD+BRS frames `can_e1m0` -> `can_e1m1` and back at driver-default timing, checks reception and that both ports stay ERROR-ACTIVE, and prints the link counters. Skips unless `/etc/alp-hil-can-cabled` exists on the target. `docs/build-yocto-v2n.md` gains the result interpretation and the `tdc-mode manual` fallback.
