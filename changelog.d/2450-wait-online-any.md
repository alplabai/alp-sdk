### Fixed

- **meta-alp-sdk:** an image with only one of the two GbE ports cabled no longer boots `degraded`. `systemd-networkd-wait-online` waited for both `end0` and `end1`, timed out on the uncabled port and failed. `alp-network-defaults` now installs a drop-in that runs it with `--any`, so one online link is enough. A board with no link at all still fails the unit. Found on E1M-V2M103 serial 2026W38-0001 with the release-candidate image (#2450).
