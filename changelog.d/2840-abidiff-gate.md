### Added — abidiff gate on the built libalp_sdk.so (#2840)

`scripts/check_abidiff.py` (CI: `pr-plain-cmake.yml:abidiff`) builds `libalp_sdk.so` from the
latest release tag of the current minor (else that minor's latest rc tag) and from the PR head
with the yocto shared configuration, and fails on any `abidiff` ABI_CHANGE or
ABI_INCOMPATIBLE_CHANGE inside the minor line, so patch releases stay safe for in-place
package upgrades. A missing `abidiff` skips loudly locally; CI passes `--require-tool`.
