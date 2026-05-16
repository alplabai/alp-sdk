# Tutorial 12: Mender OTA on Yocto

**Target audience:** developers who already have a V2N or
i.MX 93 Yocto image working on the bench and want to add
in-field firmware updates.

**Prerequisites:**

- Tutorial [01](01-first-build.md) completed.
- A V2N / V2N-M1 / i.MX 93 module running a `meta-alp` Yocto
  image.
- A reachable Mender server.  For development: stand up
  Mender's open-source server locally (15 minutes; instructions
  below).  For production: a hosted Mender instance.

**Outcome:** a working A/B-partition OTA flow.  Push an updated
rootfs from the Mender server; the device fetches + verifies +
swaps partitions + reboots into the new image.  An interrupted
update reverts cleanly.

**Scope note:** this is **Yocto-side** OTA only.  AEN-Zephyr
OTA is deferred to v1.1 per [ADR 0009](../adr/0009-mender-zephyr-client-deferred.md);
the Mender Zephyr-client decision is pending real customer pull.

**Time:** 60–90 minutes the first time (server setup + first
artefact build + first update); ~5 minutes for subsequent
updates.

---

## 1. Stand up a Mender server (30 minutes)

For development, use the open-source `mender-server` running in
Docker on your laptop.  Production uses Mender's hosted SaaS
or a self-hosted enterprise install.

```bash
# Clone + start the integration repo's docker-compose stack.
git clone https://github.com/mendersoftware/integration -b master
cd integration

./demo up
```

The `demo` script brings up: mender-server, mender-deployments,
mender-deviceauth, mender-inventory, NGINX TLS proxy, and a
mongo store.  Takes ~5-10 minutes the first time.

When it's up, open `https://localhost` in a browser, accept the
self-signed cert, and create the initial admin user when
prompted.  Save the credentials.

> **Production**: use Mender's hosted server
> ([hosted.mender.io](https://hosted.mender.io)).  Same client
> protocol; the URL changes.

## 2. Enable Mender in your `meta-alp` build (5 minutes)

```yaml
# In your board.yaml (schema_version: 2):
cores:
  a55_cluster:
    app: ./linux                    # os: omitted -- A-cores default to yocto per topology

# Add an OTA block under `features:` (schema's free-form
# extension point for app-level concerns the strict-typed
# blocks above don't cover):
features:
  ota:
    client:    mender
    server:    https://localhost    # or hosted Mender URL
    tenant:    default              # multi-tenancy: set per customer
    rootfs_ab: true                 # A/B partition layout
```

The loader threads `features.ota:` into the meta-alp-sdk Mender
config block.  The matching `meta-alp-sdk/conf/distro/include/mender.inc`
turns into the right `IMAGE_FSTYPES` + Mender `inherit` lines.

## 3. Build a signed image artefact (10 minutes)

```bash
source poky/oe-init-build-env build-v2n
# bitbake-layers add-layer ../alp-sdk/meta-alp-sdk  (if not already added)
MACHINE=e1m-v2n101-a55

# Build a Mender-aware image (instead of plain
# core-image-minimal):
bitbake core-image-minimal-mender
```

Output: `tmp/deploy/images/e1m-v2n/core-image-minimal-mender-e1m-v2n.mender`.
This is the **Mender artefact** -- a tar archive containing the
rootfs blob + metadata + the signature.

Flash the initial image to microSD or eMMC:

```bash
sudo dd \
    if=tmp/deploy/images/e1m-v2n/core-image-minimal-mender-e1m-v2n.wic \
    of=/dev/sdX bs=4M conv=fsync
```

## 4. First device boot + provision

Boot the device.  The Mender client (auto-started by systemd)
contacts the server and requests authorization.  In the Mender
UI:

1. **Pending Devices** tab shows the new device with its
   serial / hw_rev (read from the EEPROM manifest -- see
   Tutorial [Tutorial: EEPROM provisioning](13-eeprom-provisioning.md)).
2. Click **Accept**.  The device receives its token + becomes
   visible under **Devices**.

Verify the device-side reads the manifest correctly:

```bash
# On the device:
journalctl -u mender-client -n 50
# Expect: "Authentication accepted", "Device deployment status: ..."
```

## 5. Build + deploy an updated image (5 minutes)

Make a small change in your meta-alp recipes (e.g. bump a
package version, add a debug log).  Rebuild:

```bash
bitbake core-image-minimal-mender
```

Upload the new `.mender` artefact via the UI's **Releases**
tab, then create a **Deployment** targeting your device group.

Watch the device pick it up:

```bash
# On the device:
journalctl -u mender-client -f
# Expect:
# - "Update available"
# - "Downloading update"
# - "Installing update"
# - Reboot
# - "Update committed" (after the post-reboot health check)
```

If the new image boots cleanly + the health check passes, the
swap is committed.  If not, the device reverts to the previous
partition automatically.

## 6. Verify the rollback path

Force a bad image:

1. Build a Mender artefact that panics on boot (e.g. an
   `image_postprocess_command` that corrupts `/etc/inittab`).
2. Deploy.
3. The device downloads + installs + reboots + panics +
   reboots again.

Mender's `confirm` step runs only after the OS reports
healthy.  An image that panics or hangs the watchdog never
gets confirmed → next boot reverts to the previous partition.
The Mender UI shows the deployment as `FAILED -- reverted`.

## 7. Production checklist

- [ ] Production server (hosted Mender or self-hosted
      enterprise).  Don't ship to the field pointed at a
      laptop-localhost server.
- [ ] Real CA-signed TLS cert on the Mender server.  Pin the
      CA bundle in `meta-alp`'s Mender client config.
- [ ] Sign all artefacts (Mender's `mender-artifact write`
      `--key-pair` mode).  Verify on the device side via the
      Mender client's `ArtifactVerifyKey` config.
- [ ] Deployment phases: 1 % canary → 10 % beta → 100 %.  Use
      Mender's deployment phasing UI.
- [ ] Monitor the device fleet's `update_status` + the
      `last_check_in` timestamps in inventory.
- [ ] Have a "freeze updates" toggle at the server level for
      incident response.

## 8. AEN-Zephyr OTA (v1.1+)

As of v0.4, AEN-Zephyr boots signed images via MCUboot but has
**no in-field OTA path** -- the Mender Zephyr client decision
is deferred per [ADR 0009](../adr/0009-mender-zephyr-client-deferred.md).
Workarounds in the v0.4..v1.0 window:

- Physical-access reflash via J-Link / OpenOCD.
- Carrier-side OTA fronted by a Linux companion (e.g. a V2N SoM
  coordinating the AEN-Zephyr update through the GD32 bridge).

The v1.1 cycle picks `mender-mcu-client` vs Hawkbit + ships the
matching Zephyr-side helper.

## See also

- [`docs/ota.md`](../ota.md) -- design + trust model.
- [`docs/ota-device-contract.md`](../ota-device-contract.md) --
  device-side contract.
- [`meta-alp-sdk/conf/distro/include/mender.inc`](../../meta-alp-sdk/conf/distro/include/mender.inc)
  -- the Mender opt-in block.
- [Mender docs](https://docs.mender.io/) -- upstream
  reference for the server + client protocols.
