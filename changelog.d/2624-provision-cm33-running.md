### Added

- The provisioning functional test gains `cm33_running`: it reads the stock CM33 image's liveness beacon (magic, version, counter at A55 `0x4F700FF0`) through `devmem` or a python3 `/dev/mem` read, and requires the counter to advance within 2 s. Read-only; blocking when the bundle carries a `cm33` component, informational otherwise.
