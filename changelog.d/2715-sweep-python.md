### Fixed — script hardening from the Python sweep

- **`select_checks.py`** now answers `full` when a change deletes a `scripts/` file, instead of `skip`.
- **`gen_example_alp_conf.py`** removes a stale per-SKU `generated/<sku>/alp.conf` before regenerating.
- **`provision_som.py`** (V2N flow) validates and resolves the HiL spec path before the serial is allocated.
- **`provision/linux_target.py`** decodes board output as UTF-8 with replacement instead of the host locale.
