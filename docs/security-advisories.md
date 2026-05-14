# Security advisories

How to report a security vulnerability in the ALP SDK + how we
handle reports + how customers find out about embargoed fixes.

## Reporting a vulnerability

**Use GitHub's private Security Advisory channel:**
[**https://github.com/alplabai/alp-sdk/security/advisories**](https://github.com/alplabai/alp-sdk/security/advisories)

Click **"Report a vulnerability"**.  The form is private --
the report is visible only to you + the SDK maintainers.

**Do NOT:**
- File a public issue in the GitHub tracker.
- Post on [`community.alplab.ai`](https://community.alplab.ai/).
- Email the maintainers directly with technical details
  (encrypted comms via the GitHub flow are safer than ad-hoc
  email).

## What we treat as a vulnerability

The ALP SDK is a unification layer over vendor SDKs.  Issues we
treat as security advisories:

| Category | Examples |
|----------|----------|
| Privileged-API memory safety | Buffer overrun in `alp_audio_in_read`, OOB in `alp_mqtt_publish` payload handling, missing length-check in an `alp_*_open` path. |
| Cryptographic correctness | `<alp/security.h>` returning attacker-controlled bytes from `alp_random_bytes`; AEAD tag mismatch not enforced; weak default IVs. |
| Secure-boot bypass | MCUboot signature validation skip on the AEN-Zephyr path; downgrade attack against `swap-using-scratch`. |
| OTA update integrity | Mender update accepted without signature verification on V2N-Yocto. |
| Secure element misuse | OPTIGA Trust M key material exposed through a public `<alp/X.h>` symbol. |
| Privilege escalation across cores | Mproc IPC framing parser RCE from a malicious peer M55 image. |
| `board.yaml` loader RCE | `scripts/alp_project.py` arbitrary code execution from a crafted manifest. |

The following are **bugs, not vulnerabilities** (file as
regular issues):

- Wrong return code on an error path.
- A backend stub returning `NOSUPPORT` when the customer expected
  real impl.
- Missing `@brief` doxygen on a public symbol.
- A test flakes intermittently.

If you're not sure which category an issue falls into, use the
advisory channel anyway -- mis-categorising as security is
strictly safer.

## Our response process

1. **Triage within 5 business days.**  A maintainer confirms the
   advisory is reachable + reproduces the issue (or asks for more
   info).  Status reflected in the advisory thread.
2. **Severity assignment** using CVSS 3.1.  We aim for honest
   scores -- "high" doesn't always mean "panic"; "low" doesn't
   mean "ignore".
3. **Fix development under embargo.**  The fix lands on a private
   fork branch + cherry-picks to every supported release branch
   (e.g. `release/v1.0` for LTS).  No public commits during the
   embargo.
4. **Coordinated disclosure.**  Default embargo: 90 days from
   first reporter response, or until customers have had **30
   days** with the fix available (whichever is sooner).
5. **Publication.**  Advisory text published with CVE assignment
   (if applicable), patched-version range, mitigation steps for
   customers who can't upgrade immediately.

## Backport coverage

When a fix lands, we backport to every release branch that's still
within its support window per
[`docs/release-policy.md`](release-policy.md):

- All currently-supported LTS branches.
- The two most recent non-LTS minor releases (`v1.N` + `v1.N-1`).

We do **not** backport beyond the support window; customers on
end-of-life versions get advised to upgrade.

## Reporter credit

Reporters who find genuine security issues get acknowledged by
name + handle in the published advisory unless they opt out.
We don't run a paid bug-bounty program at v1.0 timeframe --
acknowledgement is the only form of credit.

## Severity rubric (operational shortcut)

| Severity | CVSS  | Default embargo | Backport reach |
|----------|-------|-----------------|----------------|
| Critical | 9.0+  | 7 days          | Every supported LTS + every supported minor |
| High     | 7.0-8.9 | 30 days       | Every supported LTS + most recent two minors |
| Medium   | 4.0-6.9 | 60 days       | Most recent LTS + most recent minor |
| Low      | < 4.0 | 90 days         | Most recent LTS only |

## Out of scope

- **Vendor-SDK CVEs we wrap** (Alif, Renesas, NXP, DEEPX, TI
  SimpleLink).  We track upstream advisories and bump our pins
  but the original report flow is the vendor's.
- **Zephyr / Yocto / MbedTLS / OpenSSL CVEs**.  Same -- track
  upstream, bump pins, advise customers when our matrix moves.
- **Customer-side application bugs**.  We can't audit application
  code; this advisory channel is for SDK-side issues.
