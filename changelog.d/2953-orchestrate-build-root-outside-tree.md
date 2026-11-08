### Fixed — orchestrate: an absolute `--build-root` outside the project and the SDK is rejected (#2953)

`--emit build-plan` with an absolute `--build-root` under neither the project nor the SDK checkout could not tokenise the build-dir-anchored arguments, so every zephyr/baremetal slice got `command: null` and exit 0; a consumer honouring `executionPolicy.nullCommand: skip` then reported a green build that built nothing. The emit now fails with a clear error (exit 1); relative build roots and roots under the project or the SDK are unchanged.
