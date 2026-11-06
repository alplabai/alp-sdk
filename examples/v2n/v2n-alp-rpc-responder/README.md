# v2n-alp-rpc-responder

The E1M-V2N101 / V2M101 Cortex-M33 as an `<alp/rpc.h>` responder for the Linux
A55: it opens channel `alp_v2n_echo`, subscribes to `echo` and sends every
payload straight back.

`board.yaml` declares an `ipc:` rpmsg entry between `a55_cluster` and `m33_sm`;
the orchestrator turns that into `CONFIG_ALP_SDK_RPC_LINK=y`, which pulls in the
`alp,ipc-v2n-rpmsg` ipc_service backend (`zephyr/drivers/ipc/`). The backend
publishes the Alp AMP beacon from boot, which the A55 backend
(`src/backends/rpc/yocto_uio_drv.c`) needs before it attaches.

```bash
cd examples/v2n/v2n-alp-rpc-responder
tan build
```

Or by hand, after `python3 scripts/gen_example_alp_conf.py examples/v2n/v2n-alp-rpc-responder`:

```bash
west build -b alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33 examples/v2n/v2n-alp-rpc-responder \
    -- -DEXTRA_CONF_FILE=generated/alp.conf
```

On the A55, open the same channel name with `alp_rpc_open()`, set
`dst_ept = 1024` and `alp_rpc_call()` method `echo`. `ipc_service` has no
address field, so the M33 ignores `src_ept` / `dst_ept` and serves its Nth open
channel (first free slot) at rpmsg address 1024 + N (this one is the first, so
1024). The link reports up once the A55's first frame arrives, and sends are
non-blocking (`ALP_ERR_NOMEM`-class failure when no TX buffer is free).

**Status:** build-verified only. Bench-unverified through `<alp/rpc.h>`; the
transport underneath is the one `examples/multicore/rpmsg-v2n` proved on
silicon.
