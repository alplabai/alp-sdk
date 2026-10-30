### Changed — canopennode no longer relies on Zephyr optional.yaml (#2995)

Zephyr 4.5 removes canopennode from submanifests/optional.yaml and makes
CANopenNode an external module. The manifest already carries a
self-contained integration.zephyr.west pin (url, SHA, path) emitted via
west-libraries; reword the header, README and tests so the premise is that
own pin, not Zephyr's optional group. Still valid on v4.4.1. Grounding
provenance comments are unchanged.
