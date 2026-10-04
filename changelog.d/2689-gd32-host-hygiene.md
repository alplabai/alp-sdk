### Changed — GD32 bridge host-side hygiene: cached version, N-sample ADC burst, I2C error replies, multi-line GPIO

**`gd32g553_get_version()` is served from the cache `gd32g553_init()` fills**,
so the console `ver` command and portable-API callers no longer send
`GET_VERSION` on every call. The cache is dropped on re-init/deinit, after
`gd32g553_ota_commit()` / `gd32g553_ota_rollback()` (the bridge reboots into a
possibly different image), and on any `ALP_ERR_IO` / `ALP_ERR_TIMEOUT` from the
link. New `gd32g553_refresh_version()` always goes to the wire for callers that
use `GET_VERSION` as a link probe (the V2N bridge examples now do).

**New `alp_adc_read_raw_n()` burst read.** On V2N the GD32 backend fetches up to
8 samples per `ADC_READ` round trip (`ceil(n / 8)` trips) instead of one trip per
sample; other backends fall back to `n` x `alp_adc_read_raw()`. The V2N ADC
backend now advertises `max_rate_hz = GD32G553_BRIDGE_ADC_STREAM_MAX_RATE_HZ`
(100 kHz, the streaming path's ceiling) instead of `0`, so callers can discover
`alp_adc_stream_open()`. No wire-protocol change.

**Linux `gpio-gd32-bridge` driver** (`0005-gpio-add-gd32-bridge-expander-driver.patch`):
a bridge error reply on I2C is `[STATUS][CRC]` (3 bytes); the driver used to
read it at full width and report `STATUS_BUSY` as `-EBADMSG` "reply CRC
mismatch". It now decodes the short reply and maps statuses to errnos
(`BUSY` -> `-EBUSY` with a bounded retry, `NOT_READY` -> `-EAGAIN`, `TIMEOUT` ->
`-ETIMEDOUT`, `NOSUPPORT` -> `-EOPNOTSUPP`, ...). `.get_multiple` / `.set_multiple`
make a multi-line request one bridge transaction. The Linux driver still speaks
only GPIO and `SE_RST`.
