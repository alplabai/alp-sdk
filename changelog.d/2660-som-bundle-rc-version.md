### Changed — SoM release bundles accept a release-candidate version (#2660)

`som-release-bundle-v1.schema.json` now accepts `release_version` values of the form `som-X.Y.Z-rcN` as well as `som-X.Y.Z`, so a release candidate can be signed, validated by `check_som_bundle.py` and used by `provision_som.py` for bench provisioning before it is promoted to a release. Other suffixes are still rejected.
