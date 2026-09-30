### Fixed

- `scripts/provision_som.py` (V2N family): `--cold-cycles` below 1 is now a usage error, `cold_boot_test` fails when it observed no clean cold boot, and `secure_page_lock` refuses unless the recorded `cold_boots_passed` shows at least one boot and all requested boots clean, or when any step is failed in the state file. Before, `--cold-cycles 0` passed `cold_boot_test` with zero boots and satisfied the irreversible lock precondition (#2464).
