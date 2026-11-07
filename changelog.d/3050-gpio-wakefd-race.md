### Fixed — Yocto GPIO IRQ dispatcher no longer races a restart on its wake eventfd (#3050)

**`src/yocto/peripheral_gpio.c` read and wrote `g_irq.wake_fd` without `g_irq.mu` while a dispatcher restart could rewrite it.** The dispatcher now drains the fd it polled (snapshotted under the lock), and `irq_wake()` snapshots the fd under the lock before writing. `alp_test_can_gpio_tsan` failed intermittently under ThreadSanitizer (3 of 5 runs) and is clean in 10 of 10 runs with the fix.
