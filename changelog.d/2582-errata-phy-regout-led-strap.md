### Documentation — two new V2N/V2M errata: PHY regulator output capacitor and carrier link LED on a PHY strap (#2582)

`docs/errata-e1m-x-v2n.md` gains E4 and E5:

- **E4 (module):** a 0.1 µF capacitor on each Ethernet PHY's regulator output pin overloads the PHY's internal switching regulator (Realtek's reference leaves it unfitted for that configuration). Symptom: about 0.45 A at 15 V once the PHYs leave reset, hot PHYs, the PHY core rail near 0.3 V and `Failed to reset the dma` on both ports. Fix: remove the capacitor on both PHYs; confirmed on two units.
- **E5 (carrier):** the RJ45 green LED loads the PHY's `LED0` configuration strap during the reset-latch window. Fix: remove the LED's series resistor on both ports.
