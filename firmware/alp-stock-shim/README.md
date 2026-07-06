# alp-stock-shim

Minimal Zephyr image for SoM preset M-core defaults that use
`app: alp-stock-shim`.

The shim intentionally does not claim peripherals or IPC endpoints. It gives
the orchestrator a buildable, bootable peer-core image when a project leaves a
secondary M-core at the SoM default, while customer applications can still
override `cores.<id>.app` with their own firmware.
