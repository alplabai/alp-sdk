### Changed — stop reading uart_irq_update() return in the RX ISR drain (#2995)

The RX ring-buffer ISR looped on 'uart_irq_update(dev) > 0 && ...'.
uart_irq_update() returns int on Zephyr v4.4 but void from v4.5, so the
loop condition fails to compile on 4.5. Call it once per callback to
latch the IRQ status, then loop on uart_irq_rx_ready(), which behaves
identically on both versions.
