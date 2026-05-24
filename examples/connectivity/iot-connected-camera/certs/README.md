# certs/

TLS material for the MQTT-over-TLS path.  v0.3 deliverable.

## v0.1 status

Empty.  v0.1 doesn't compile MbedTLS; the IoT stub returns
`ALP_ERR_NOSUPPORT` for every connect call regardless of cert
presence.

## What goes here (v0.3)

- `ca.pem` — broker root CA bundle (PEM).  Required.
- `client.pem` — client certificate (PEM).  Optional, for mutual-TLS.
- `client.key` — client private key (PEM, unencrypted).  Required if
  `client.pem` is provided.

`prj.conf` for v0.3 turns on:

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

- **Never** commit private keys to git — `client.key` is in
  `.gitignore` for this directory.
- Use a per-device certificate when shipping at any scale; reuse
  is a security smell.
- Rotate the CA bundle when your broker rotates.  v0.3.x will ship
  a `cert-pull` helper that fetches the bundle from a provisioning
  endpoint at first boot.

## Dev / test broker

For development, run an Eclipse Mosquitto broker locally:

```bash
docker run --rm -p 8883:8883 \
    -v $(pwd)/dev-broker.conf:/mosquitto/config/mosquitto.conf \
    -v $(pwd)/certs:/certs \
    eclipse-mosquitto:2
```

Sample `dev-broker.conf` lands in v0.3 alongside this README.
