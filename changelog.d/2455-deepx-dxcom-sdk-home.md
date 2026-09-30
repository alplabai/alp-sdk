### Fixed — DEEPX compile now runs the `dxcom` that `is_available()` found (#2455)

`is_available()` accepted `ALP_DEEPX_SDK_HOME`, but `compile()` ran a bare `dxcom` from `PATH`, so with the SDK reachable only through that variable the build died with `FileNotFoundError`. One resolver (`_dxcom_exe()`) now serves both, looking on `PATH` and then in `bin/`, `Scripts/` and the root of `ALP_DEEPX_SDK_HOME`. A missing executable raises the adapter's normal `RuntimeError` with a message naming both remedies.
