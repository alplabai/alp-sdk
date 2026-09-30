### Fixed — GD32 bridge GPIO kernel driver: use-after-free on unbind and SE_RST left asserted

`gpio-gd32-bridge` cancelled its replay and resolve works before the gpiochip
was removed, so a consumer still holding a line (`wlan-pwrseq`, `hci_bcm`, the
panel reset, a userspace line fd) could call `.set` after the cancel, re-queue
`replay_work`, and have it run on freed driver data. A `removing` flag, set
under `state_lock` by one teardown action that devm now runs before
`gpiochip_remove()`, stops any further queueing before both works are
cancelled synchronously. The same teardown releases `SE_RST` (line 21) if a
consumer left it asserted, and `gpio_free()` now retries the release a bounded
number of times and warns on failure instead of silently leaving the Trust M
held in reset.
