### Changed — build the xHCI UHC driver on Zephyr 4.4.1 and 4.5.0 (#2995)

Zephyr 4.5 renames struct uhc_api to struct uhc_driver_api with no alias
and requires the op table to be declared with DEVICE_API(uhc, ...), so
uhc_xhci_alif.c would fail to compile after the pin flip. Guard the table
declaration on ZEPHYR_VERSION_CODE so it builds on both v4.4.1 and
v4.5.0; the comment names the 4.5 migration-guide change so the guard
can be deleted once the pin moves.
