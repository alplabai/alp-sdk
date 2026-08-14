# Secure boot on E1M-AEN

This document describes the chain of trust from immutable ROM
through to the application on E1M-AEN-family SoMs, plus the
signing key lifecycle that makes it work.

> **Status: boot + signature verification bench-proven on
> E1M-AEN801 (single-slot); swap-using-scratch's swap/rollback path
> is untested.**  The chain of trust below -- SES -> MCUboot (ITCM)
> -> slot0 (MRAM XIP) -> application -- is measured working on real
> silicon (`AE822FA0E5597LS0` Rev A0, alp-sdk `0da1f1b4`), with
> `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y` and
> `CONFIG_BOOT_VALIDATE_SLOT0=y` read back from the built
> `mcuboot/zephyr/.config` rather than assumed: `PC=80012FBC`,
> `VTOR=80010800`, `CFSR=00000000`, `IPSR=000`.  Verification was
> proven live, not inferred from a clean boot: flipping one byte of
> the TLV `0x22` ECDSA signature (file offset `0x4a30`, `0xda` ->
> `0xdb`, with TLV `0x10`/SHA-256 and TLV `0x01`/key left intact)
> produces `D: bootutil_verify_sig: ECDSA builtin key 0` then
> `E: Unable to find bootable image`, `VTOR = 0x00000000` -- a halt,
> not a swap-back to a previous slot (this build had none:
> `CONFIG_SINGLE_APPLICATION_SLOT=y`).  `SIGNATURE_TYPE_NONE` +
> `VALIDATE_SLOT0=y` boots in twelve seconds (watched ten minutes,
> CycleCnt advancing).
>
> **A separate build on the reference `swap-using-scratch` profile
> boots and logs `I: Bootloader chainload address offset: 0x10000` --
> boot only.  The swap/rollback path itself (failed-image swap-back,
> mid-swap power loss recovery, revert-on-unconfirmed) was NOT
> exercised** -- see "Failure modes + rollback" below.
>
> The verified backend is **TinyCrypt** (`CONFIG_BOOT_ECDSA_TINYCRYPT=y`),
> not yet MbedTLS PSA.  The built `.config` also confirms
> `CONFIG_SINGLE_APPLICATION_SLOT=y` and `CONFIG_FLASH_BASE_ADDRESS=0x0`
> (MCUboot itself is ITCM-linked).  Still required: `CONFIG_DCACHE=n`
> (a separate, established hang in `SCB_EnableDCache`), the board's
> `ROM_START_OFFSET=0x800`, and the `zephyr/patches/mcuboot`
> `do_boot` flash-base patch (a candidate for upstreaming rather than
> carrying indefinitely).  Compile-verification also gates on the real
> in-tree board file (`alp_e1m_aen801_m55_he` / `alp_e1m_aen801_m55_hp`,
> under [`zephyr/boards/alp/`](../zephyr/boards/alp/)).
>
> **The customer path is now proven too.**  A plain J-Link `loadbin` of
> an `imgtool`-signed image straight to slot0 (`0x80010000`) -- no
> SETOOLS, no ATOC, no SE-UART -- is verified by MCUboot and
> chainloaded, and survived three cold power-cycles.  **Proven at
> `0x80010000` only** -- writing the ATOC region or erasing MCUboot
> itself was not tested.  Both refusal shapes (tampered signature, or a
> non-MCUboot image) leave the debug port alive (`Secure debug:
> enabled`, core halts and single-steps normally) -- a bad slot0 write
> does not brick J-Link access.  This is a **single-slot** result
> (`CONFIG_SINGLE_APPLICATION_SLOT=y`); A/B swap and OTA are untested,
> so don't read it as an upgrade-path guarantee.  Full recipe:
> [`docs/aen-provisioning.md`](aen-provisioning.md) §0.5.

## Chain of trust

```
┌─────────────────────────────────────────────────────────────┐
│ Alif Ensemble Secure Enclave ROM                            │
│   - Immutable.  Verifies the first-stage bootloader.        │
│   - Roots its trust in an Alif-fab-time-burned public key.  │
├─────────────────────────────────────────────────────────────┤
│ First-stage bootloader (Alif-provided)                      │
│   - Hands off to MCUboot.                                   │
├─────────────────────────────────────────────────────────────┤
│ MCUboot (alp-sdk-built via sysbuild)                        │
│   - Verifies the application image's ECDSA-P256 signature   │
│     against the public key compiled into the bootloader.    │
│   - Failed verification triggers swap-back to the previous  │
│     slot (swap-using-scratch mode).                         │
│   - Public key verification path routes through MbedTLS PSA │
│     once the v0.3.x OPTIGA Trust M PSA driver lands, so HW  │
│     acceleration is transparent.                            │
├─────────────────────────────────────────────────────────────┤
│ Application image                                           │
│   - Signed by the production private key held on the        │
│     air-gapped signing workstation.  (In-chip custody in    │
│     OPTIGA Trust M is the intended end state -- the driver  │
│     is probe-only today; see "Signing key lifecycle".)      │
└─────────────────────────────────────────────────────────────┘
```

> **`[UNTESTED]` -- the "Failed verification triggers swap-back to
> the previous slot" bullet above.**  Neither bench session ran
> swap-using-scratch's two-slot swap-back; the single-slot build that
> was tested has no previous slot to fall back to, and measured
> `E: Unable to find bootable image` / `VTOR = 0x00000000` -- a halt,
> not a swap.  See the status block above and "Failure modes +
> rollback" below.

The SDK touches the MCUboot layer.  The Alif Secure Enclave
ROM + first-stage are out of scope -- they ship with the SoM
and Alif provides their signing keys.

## Declarative wiring (`boot:` block in `board.yaml`)

The recommended path is the top-level `boot:` block in your
project's `board.yaml` -- the loader (`scripts/alp_orchestrate/`)
emits the matching `SB_CONFIG_*` overlay (sysbuild Kconfig) into
`build/alp_sysbuild.conf` and passes it via
`-DSB_CONF_FILE`.  No hand-edited sysbuild.conf.

```yaml
# board.yaml
boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/prod_ecdsa_p256.pub.pem
  swap_algorithm: scratch   # valid on a two-slot target; omit (or drop
                             # this line) on a single-slot target such
                             # as E1M-AEN801 -- see below, it errors.
```

See [`docs/board-config-features.md` §Bootloader](board-config-features.md#bootloader-boot----mcuboot)
for the full field reference (including why there is no
`slots:` / `scratch_size_kib:` / `anti_rollback:` field).

**Omit the `boot:` block entirely** to inherit the SDK's stock
per-family defaults unchanged: on AEN-Zephyr that is the curated
[`zephyr/sysbuild/aen/sysbuild.conf`](../zephyr/sysbuild/aen/sysbuild.conf)
alone (MCUboot + ECDSA-P256 +
`SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`) -- this base applies to every
AEN SKU, not just a single-slot one, since no `boot:` block means the
loader emits no overlay at all for it to layer against.

**Keep the `boot:` block but omit `swap_algorithm:`** and the default
instead follows the *target's own DT*, not one value for every SKU: a
target whose `memory_map:` declares a disjoint per-core `<role>_slot0`
region (#1069: both M55 cores share the same physical App MRAM, so
slot0 was split into disjoint per-core windows and the
secondary/scratch slot dropped rather than forced to fit -- since #1445
that is every AEN SoM, not just **E1M-AEN801**) has no slot1/scratch
partition, so the generated overlay resolves to single-app boot
(`SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`, the same symbol the curated
base above already ships for it); a target with no such region keeps
the historical swap-using-scratch default
(`SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y`) -- and since the generated
overlay is layered AFTER the curated base (#807: same `;`-joined
`SB_CONF_FILE` list, later file wins on a repeated symbol), presence
of a `boot:` block is what makes that scratch default apply even
though the curated base's own default is single-app.  Setting
`swap_algorithm: scratch` (or `move`/`overwrite`) explicitly on a
single-slot target such as E1M-AEN801 is a build-time error -- there
is no slot1/scratch partition for it to use (#1413).

Keeping the block but omitting `method:` inherits the family's
bootloader strategy the same way -- `mcuboot` on AEN/N93, `none` on
V2N/V2N-M1, where U-Boot owns boot.  This overlay is a Zephyr
artefact: a project with no `os: zephyr` slice never runs sysbuild and
gets none.  Slot/scratch partition *sizes* come from the board DT
`partitions {}` node, not from `boot:` -- declare the actual layout
via `storage:` if you want it explicit.

## Signing key lifecycle

### Development

1. Clone the repo.
2. Run `bash keys/generate_dev_key.sh` once.  Generates
   `keys/mcuboot_dev_ecdsa_p256.pem` (gitignored).
3. Build with sysbuild:
   ```bash
   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
       path/to/app \
       --sysbuild \
       -- -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
   ```
   (Or, if your `board.yaml` carries a `boot:` block, the loader's
   emitted overlay at `build/alp_sysbuild.conf` is the canonical
   `-DSB_CONF_FILE` path.)
4. `build/zephyr/zephyr.signed.bin` is your signed image.
5. Flash both the MCUboot bootloader and the signed app:
   ```bash
   west flash --bin-file build/mcuboot/zephyr/zephyr.bin --domain mcuboot
   west flash --bin-file build/zephyr/zephyr.signed.bin
   ```

The dev key has signing power equivalent to "every developer
who's ever cloned the repo".  Never use it in a fielded device.

### Production

> **The OPTIGA Trust M signing path described below is NOT
> implemented.**  The SDK's OPTIGA Trust M driver is
> **probe-only**: `optiga_trust_m_init` reads the I2C_STATE
> register to confirm the part ACKs, and every other entry point
> -- product info, raw APDU transport, `CalcSign`, `GenKeyPair`,
> ECDH -- returns `ALP_ERR_NOSUPPORT` after argument validation
> (`chips/optiga_trust_m/optiga_trust_m.c`; the contract is pinned
> by `tests/scripts/test_optiga_probe_only_contract.py`).  There is
> no key generation, no key export, and no signing through the
> secure element today.  **Do not architect a product's key
> management around it** until the Infineon host-library transport
> is integrated (issue #481).
>
> Use the air-gapped workstation flow below.  The OPTIGA design is
> retained here as the intended end state, clearly marked.

Provisioning happens at SoM manufacturing time -- *before* the
device ships.

#### What to do today: air-gapped signing workstation

1. **Generate the production key on an air-gapped workstation.**
   A machine that never touches the network, holding the ECDSA-P256
   private key on encrypted storage (ideally a smartcard or HSM you
   already trust).  This machine is the key store.
2. **Publish the public half.**  Commit it as
   `keys/mcuboot_prod_ecdsa_p256.pub.pem`; the manufacturing CA
   signs it so downstream tooling can verify provenance.  Only the
   public half ever enters git.
3. **Compile the bootloader against the production public key.**
   Override `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` to point at
   `keys/mcuboot_prod_ecdsa_p256.pub.pem` when building MCUboot for
   production firmware.
4. **Sign release images across the air gap.**  Carry the unsigned
   image to the workstation, sign it there with stock `imgtool`, and
   carry `zephyr.signed.bin` back.  The release pipeline never holds
   signing power.

The security property this gives you: the private key never touches
a networked machine.  Compromise of the dev key, the build host, or
CI does not yield production signing power -- physical access to the
air-gapped workstation does.  That is a weaker guarantee than
in-chip key custody (an operator with access can copy the key), so
treat workstation access control as the control that matters.

#### Intended end state (pending #481)

Once the OPTIGA Trust M host-library transport lands, the private
key is generated *inside* the chip (slot 0xE0F0 is the convention
for the MCUboot signing key) and never leaves its secure NVM; the
public half is read out over I²C, and the release pipeline signs
through a dedicated host with physical access to a provisioned part
(`imgtool` supports external ECDSA signers via
`--public-key-format hash`).  That removes the copy-the-key risk
above.  None of it works yet.

### Key rotation

MCUboot supports compiling against multiple public keys, so rotation
does not depend on the secure element:

1. Generate the next-generation key on the air-gapped workstation.
   (End state, pending #481: provision a new OPTIGA Trust M slot,
   e.g. 0xE0F1.)
2. Compile the bootloader with both `*_pub.pem` files committed
   under `keys/`.  Signed images from either key are accepted.
3. Roll the new key out to fielded devices via OTA (signed by
   the *current* key).
4. Wait one full update window (typically 90 days).
5. Compile the bootloader with only the new key.  Roll out the
   new bootloader.  Old key is now untrusted.

The rotation cadence is policy, not a hard constraint -- the
chain stays intact regardless of cadence as long as no signed
image escapes a slot whose key is still trusted.

## Failure modes + rollback

`[UNTESTED]` -- both bench sessions ran `CONFIG_SINGLE_APPLICATION_SLOT=y`
(no secondary slot); the table below is swap-using-scratch's documented
two-slot design and has not itself been exercised.  What **is** measured:
a single-slot build with a bad signature halts (`E: Unable to find
bootable image`, `VTOR = 0x00000000`) rather than falling back to a
previous slot -- there is no previous slot in that configuration, which
also means row 1 below does not describe the single-slot case.

MCUboot in `swap-using-scratch` mode is *designed* to handle three
pathological cases:

| Failure                                | What happens                                   |
|----------------------------------------|------------------------------------------------|
| Signed image fails verification        | Boot the previous slot.  No state change.      |
| Mid-swap power loss                    | Scratch slot holds the in-flight bytes; next   |
|                                        | boot recovers either to the old or the new     |
|                                        | image atomically.                              |
| Swapped-in image crashes before        | MCUboot's "test" mark stays set; next boot     |
| `boot_set_confirmed()` lands           | swaps back to the previous slot automatically. |

`boot_set_confirmed()` is the application's signal that the
new image is healthy.  Apps that don't call it within a
documented time window get rolled back -- a watchdog-friendly
safety net for OTA.

## See also

- [`zephyr/sysbuild/aen/README.md`](../zephyr/sysbuild/aen/README.md) -- how
  to invoke the sysbuild config.
- [`keys/README.md`](../keys/README.md) -- key files + dev
  key generation.
- [`docs/cc3501e-bridge.md`](cc3501e-bridge.md) -- the CC3501E
  Wi-Fi bridge's role in OTA delivery on AEN.
- [`VERSIONS.md`](../VERSIONS.md) -- versioned roadmap; secure
  boot / OTA shipped together in v0.4.
