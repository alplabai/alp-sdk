### Added — opt-in `extras-cloud` west group pinning the AWS IoT and Azure IoT embedded-C SDKs (#382)

`west.yml` now carries `aws-iot-device-sdk-embedded-C` at the `202412.00` LTS release (MIT)
and `azure-sdk-for-c` at `1.5.0` (MIT), matching `metadata/libraries/aws-iot.yaml`
and `metadata/libraries/azure-iot.yaml`. Both sit in a new `extras-cloud` group
that is disabled by default, so `west update` stays light; enable it with
`west update --group-filter +extras-cloud`. Both libraries remain Tier B: there
is no CI build lane, no upstream Zephyr `module.yml` and no enable Kconfig symbol.
The AWS LTS repo vendors coreMQTT, coreHTTP and the other libraries as git
submodules; its `west.yml` project sets `submodules: true`, so `west update`
initialises them. An entry emitted by `--emit west-libraries` must add that key
by hand (the library manifest's `west:` block cannot carry it yet).
