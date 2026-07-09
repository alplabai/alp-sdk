"""Alp SDK command-line interface."""

from importlib import metadata as _metadata
from pathlib import Path

# Distribution name declared in pyproject.toml's [project] table.
_DIST_NAME = "alp-sdk-cli"


def _version() -> str:
    """Resolve the CLI version from the single source of truth.

    The version is never hand-written here.  Prefer the repo's
    ``metadata/sdk_version.yaml`` when running from a source checkout --
    it is the single source ``pyproject.toml`` is bumped from, and it
    always reflects the checkout HEAD (an editable install bakes its
    version at install time and would report a stale one).  When
    installed as a wheel there is no adjacent ``metadata/`` tree, so
    fall back to the packaged distribution metadata (also built from
    ``pyproject.toml``).  Both paths trace to one source, so the
    reported version can never drift out of sync with a stale literal.
    """
    # Source checkout: scripts/alp_cli/__init__.py -> repo root is parents[2].
    sdk_version = Path(__file__).resolve().parents[2] / "metadata" / "sdk_version.yaml"
    try:
        for line in sdk_version.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("version:"):
                # `version: 0.9.0` (drop any trailing inline comment).
                return stripped.split(":", 1)[1].split("#", 1)[0].strip()
    except OSError:
        pass

    # Installed wheel: no source tree, read the packaged metadata.
    try:
        return _metadata.version(_DIST_NAME)
    except _metadata.PackageNotFoundError:
        return "0+unknown"


__version__ = _version()
