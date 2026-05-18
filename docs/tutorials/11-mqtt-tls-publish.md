<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 11: MQTT publish (with TLS) end-to-end

**Target audience:** developers building a connected IoT
firmware that publishes telemetry to a cloud broker over
MQTT-TLS.

**Prerequisites:** Tutorial [01](01-first-build.md) completed.
Either a Yocto-running V2N module (preferred -- broker
roundtrip works end-to-end on the bench) or an AEN-Zephyr
EVK with Wi-Fi credentials.

**Outcome:** a working firmware that connects to a real broker
(local Mosquitto or a cloud one), publishes a JSON telemetry
payload every 10 s, subscribes to a control topic, and handles
reconnect cleanly on Wi-Fi loss.  Understand the SDK's
`<alp/iot.h>` API + the TLS pinning options.

**Time:** 30–60 minutes (broker setup is the long pole the
first time).

---

## 1. Stand up a local broker (10 minutes)

For development, run Mosquitto on your laptop.  Production is
the same flow against a cloud broker (HiveMQ Cloud, AWS IoT,
mosquitto.org public broker).

### macOS

```bash
brew install mosquitto
brew services start mosquitto

# Default config under /opt/homebrew/etc/mosquitto/
```

### Linux (Debian / Ubuntu)

```bash
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto
```

### Test the broker

```bash
# Subscribe in one terminal:
mosquitto_sub -t alp/test/#

# Publish in another:
mosquitto_pub -t alp/test/hello -m '"world"'
```

If you see `"world"` on the sub terminal: broker works.

## 2. Generate a self-signed TLS cert (10 minutes)

For dev only.  Production uses a real CA-signed cert.

```bash
openssl req -newkey rsa:2048 -nodes -keyout broker.key \
    -x509 -days 365 -out broker.crt \
    -subj "/CN=localhost"

# Mosquitto config (e.g. /etc/mosquitto/conf.d/tls.conf):
listener 8883
cafile /etc/mosquitto/broker.crt
certfile /etc/mosquitto/broker.crt
keyfile /etc/mosquitto/broker.key
require_certificate false   # MUTUAL TLS would set this to true

# Reload mosquitto.
```

Copy `broker.crt` to the device's filesystem (Yocto) or embed
into the firmware (Zephyr).  Customer-facing flow varies by
backend.

## 3. The application (Yocto)

```c
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "alp/iot.h"

int main(void) {
    alp_mqtt_config_t cfg = {
        .uri           = "mqtts://broker.local:8883",
        .client_id     = "alp-dev-001",
        .tls           = &(alp_mqtt_tls_config_t){
            .ca_path   = "/etc/alp/broker.crt",
            .insecure  = false,
        },
        .keepalive_sec = 30,
    };

    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    if (m == NULL) {
        fprintf(stderr, "mqtt open failed: last_err=%d\n",
                (int)alp_last_error());
        return 1;
    }

    if (alp_mqtt_connect(m) != ALP_OK) {
        fprintf(stderr, "mqtt connect failed\n");
        alp_mqtt_close(m);
        return 1;
    }
    printf("mqtt connected\n");

    alp_mqtt_subscribe(m, "alp/control/#", ALP_MQTT_QOS1);

    int counter = 0;
    while (1) {
        char payload[64];
        snprintf(payload, sizeof(payload),
                 "{\"counter\":%d,\"uptime\":%u}",
                 counter++, (unsigned)time(NULL));
        alp_mqtt_publish(m, "alp/telemetry/dev-001",
                         payload, strlen(payload),
                         ALP_MQTT_QOS1, false);
        alp_mqtt_loop(m, 10000);   /* dispatches incoming + reconnect */
    }
}
```

## 4. `board.yaml` for Yocto

```yaml
schema_version: 2

som:
  sku: E1M-V2N101

carrier:
  name: E1M-X-EVK

cores:
  a55_cluster:
    app: ./linux                  # os: omitted -- A-cores default to yocto per topology
    image: alp-image-edge
    libraries: [mbedtls]
    iot:
      wifi: true
      mqtt: true
      tls:  true
  m33_sm:
    os: "off"                     # explicit override -- skip the M-core slice

diagnostics:
  log_level: info
```

The `iot.mqtt.tls: true` flag makes the loader emit
`PACKAGECONFIG:append:pn-libmosquitto = " ssl"` to the Yocto
build, ensuring libmosquitto links OpenSSL.

## 5. Build + flash + run

```bash
cd ~/work/alp-sdk

# Yocto:
MACHINE=e1m-v2n101-a55 bitbake alp-image-edge -k
sudo dd if=tmp/deploy/images/e1m-v2n101-a55/alp-image-edge-e1m-v2n101-a55.wic \
        of=/dev/sdX bs=4M conv=fsync

# Boot the module, login, drop the cert + binary, run:
scp build/myapp root@e1m-v2n.local:/usr/bin/
scp broker.crt root@e1m-v2n.local:/etc/alp/
ssh root@e1m-v2n.local /usr/bin/myapp
```

Expected output (on the device):

```
mqtt connected
```

And on your laptop (subscribed earlier):

```
$ mosquitto_sub -t 'alp/telemetry/#' --cafile broker.crt -h broker.local -p 8883
{"counter":0,"uptime":1715749200}
{"counter":1,"uptime":1715749210}
...
```

## 6. Handle reconnect

`alp_mqtt_loop` returns when the broker disconnects (TCP RST,
TLS abort, network drop).  The wrapper handles MQTT-level
disconnect; the application handles Wi-Fi-level reconnect:

```c
while (1) {
    if (!alp_mqtt_is_connected(m)) {
        alp_mqtt_connect(m);   // retries internally
        sleep(1);
        continue;
    }
    /* normal publish loop */
}
```

The SDK's MQTT wrapper sets `alp_last_error()` on every
non-OK return, so a customer can branch on the specific
failure (TLS-cert-mismatch, broker-rejected, network-down,
keepalive-timeout).

## 7. AEN-Zephyr variant (sketch)

The same `<alp/iot.h>` API on AEN-Zephyr; the differences are
config-level only:

- `cores.<m-core>.os: zephyr` in `board.yaml` (usually omitted --
  the SoM topology default already supplies `zephyr` for M-cores).
- TLS CA bundle embedded into the firmware via Zephyr's
  `CONFIG_MQTT_LIB_TLS_INCLUDE_CERTIFICATE_NICKNAMES` flow,
  not loaded from `/etc/`.
- Wi-Fi creds compile-time (`CONFIG_WIFI_PSK="..."`) or
  loaded from EEPROM via `<alp/hw_info.h>` at runtime.

The application `main.c` is **identical** -- that's the point
of the SDK.

## 8. Production checklist

- [ ] Use a real CA-signed cert (Let's Encrypt or your
      enterprise PKI), not the dev self-signed one.
- [ ] Pin the CA bundle in firmware -- don't trust the system
      store on production fleet.
- [ ] Use mutual TLS (client cert) for any operation that
      writes to the broker.  Client cert + priv key live in
      OPTIGA Trust M (see Tutorial [10: secure boot
      signing](10-secure-boot-signing.md) for the OPTIGA key
      provisioning flow).
- [ ] Set `alp_mqtt_config_t.client_id` per-device using the
      EEPROM manifest's serial (`<alp/hw_info.h>`).  Hardcoded
      client IDs across a fleet collide.
- [ ] Test the reconnect path against a Wi-Fi outage simulator
      (kernel module `mac80211_hwsim` on Linux is the
      cheapest).

## See also

- [`<alp/iot.h>`](../../include/alp/iot.h) -- the public API.
- [`docs/threat-model.md`](../threat-model.md) §3.1 --
  network adversary class and the TLS mitigations.
- [`tests/yocto/iot_mqtt.c`](../../tests/yocto/iot_mqtt.c) --
  the 5 TLS test cases the SDK runs in CI.
