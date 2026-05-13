# docs/doxygen/ — generated API reference

Doxygen configuration + per-library reference pages live here.  The
generator runs on every PR via `.github/workflows/pr-doxygen.yml` and on every tag
to publish the public reference.

## v0.1 deliverable

- `Doxyfile` — generates HTML from `include/alp/**/*.h` only (the
  public surface — `src/`, `chips/<part>/`, and `vendors/` headers
  are intentionally excluded).
- Per-library landing pages under `docs/doxygen/pages/`.
- CI fails the build on any Doxygen warning before tagging.

The configuration lands once the v0.1 implementation work is signed
off; until then this README marks the directory.
