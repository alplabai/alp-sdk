### Fixed — Dev-kit feed refuses a non-alp distro (#2854)

**Dev-kit feed line fails loudly under a non-alp distro.** `alp-devkit-feed.inc` derives the feed line from `ALP_SDK_VERSION` and aborts instead of silently using another distro's `DISTRO_VERSION`; the docs now say the published signature comes from `package_devkit_feed.sh`, the per-arch signing fragment is optional, and which six machines get a feed (#2854).
