### Changed — raise CMake floor to 3.28, add hal_alif pin site to the bootstrap gate (#2995)

Zephyr 4.5 requires CMake >= 3.28, but the documented floor was 3.20 and
bootstrap.json carried no CMake floor at all. Add
prerequisites.cmakeMinVersion (3.28) and have check_bootstrap_manifest.py
police the getting-started.md CMake row. The Ubuntu 22.04 docs (apt CMake
3.22) now say how to get a newer one.

hal_alif (v2.3.0) was pinned in west.yml with no gate site, so it could
drift from docs/zephyr-version-policy.md unnoticed. Add halAlif.version to
bootstrap.json and its schema, a west.yml revision check and a --fix
rewrite site, with tests. Policy step 2 now requires hal_alif to build and
bench-pass against the pinned Zephyr rather than merely target it.
