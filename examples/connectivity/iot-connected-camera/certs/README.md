# certs/

TLS material for the MQTT-over-TLS path (target; not wired up yet).

## Current status

Empty.  The skeleton doesn't compile MbedTLS; the IoT stub returns
`ALP_ERR_NOSUPPORT` for every connect call regardless of cert
presence.

## What goes here

- `ca.pem` — broker root CA bundle (PEM).  Required.
- `client.pem` — client certificate (PEM).  Optional, for mutual-TLS.
- `client.key` — client private key (PEM, unencrypted).  Required if
  `client.pem` is provided.

Once the TLS path lands, `prj.conf` turns on:

```
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_TLS_LIBRARY=y
CONFIG_MBEDTLS_BUILTIN=y
CONFIG_NET_SOCKETS_TLS=y
```

The build system reads PEM bytes via `<zephyr/sys/storage_init.h>`
and registers them with `tls_credential_add()` before the MQTT
client opens its socket.

## Licensing / hygiene

- **Never** commit private keys to git — the `.gitignore` in this
  directory ignores `*.key` and `*.priv` alongside the generated cert
  material (`*.pem`, `*.crt`, `*.cer`, `*.der`, `*.csr`, `*.p12`,
  `*.pfx`), so `client.key` stays untracked.  `README.md` and that
  `.gitignore` are the only tracked files here.
- Use a per-device certificate when shipping at any scale; reuse
  is a security smell.
- Rotate the CA bundle when your broker rotates.

## Dev / test broker

For development, run an Eclipse Mosquitto broker locally:

```bash
docker run --rm -p 8883:8883 \
    -v $(pwd)/dev-broker.conf:/mosquitto/config/mosquitto.conf \
    -v $(pwd)/certs:/certs \
    eclipse-mosquitto:2
```

The `dev-broker.conf` in that command is yours to write: a Mosquitto
listener on 8883 pointing at the `ca.pem` and server cert/key under `/certs`.
