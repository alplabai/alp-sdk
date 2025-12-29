/**
 ********************************************************************************
 * @file    getting_started.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Getting Started Example - Demonstrates ALP SDK with Mock Driver
 * @note    This example works WITHOUT any vendor SDK!
 *          Perfect for learning the API and testing on your PC.
 ********************************************************************************
 */

#include "alp_spi_vft.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Main function - Getting Started with ALP SDK
 */
int main(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║         ALP SDK - Getting Started Example                 ║\n");
    printf("║         Mimari 1: VFT Pattern + Mock Driver               ║\n");
    printf("║                                                            ║\n");
    printf("║  🎯 This example runs WITHOUT any vendor SDK!             ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    // Step 1: Create Mock SPI handle
    printf("Step 1️⃣: Creating Mock SPI handle...\n");
    alp_spi_handle_t *spi = alp_spi_create_mock(0);
    if (!spi) {
        printf("❌ Failed to create SPI handle\n");
        return -1;
    }
    
    // Step 2: Configure SPI
    printf("\nStep 2️⃣: Configuring SPI...\n");
    alp_spi_config_t config = {
        .instance = 0,
        .baudrate = 1000000,        // 1 MHz
        .data_bits = 8,             // 8-bit data
        .mode = ALP_SPI_MODE_MASTER,
        .clock_mode = ALP_SPI_CPOL0_CPHA1,
        .bit_order = ALP_SPI_MSB_FIRST,
        .ss_mode = ALP_SPI_SS_MASTER_UNUSED
    };
    
    alp_status_t ret = alp_spi_init(spi, &config, NULL);
    if (ret != ALP_STATUS_OK) {
        printf("❌ Failed to initialize SPI: %d\n", ret);
        alp_spi_destroy(spi);
        return -1;
    }
    
    // Step 3: Send data
    printf("\nStep 3️⃣: Sending data...\n");
    uint8_t tx_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0xAA, 0xBB, 0xCC};
    ret = alp_spi_send(spi, tx_data, sizeof(tx_data));
    if (ret != ALP_STATUS_OK) {
        printf("❌ Failed to send data: %d\n", ret);
        alp_spi_deinit(spi);
        alp_spi_destroy(spi);
        return -1;
    }
    
    // Step 4: Receive data (loopback in mock driver)
    printf("\nStep 4️⃣: Receiving data...\n");
    uint8_t rx_data[8] = {0};
    ret = alp_spi_receive(spi, rx_data, sizeof(rx_data));
    if (ret != ALP_STATUS_OK) {
        printf("❌ Failed to receive data: %d\n", ret);
        alp_spi_deinit(spi);
        alp_spi_destroy(spi);
        return -1;
    }
    
    // Step 5: Verify loopback
    printf("\nStep 5️⃣: Verifying loopback...\n");
    printf("TX Data: ");
    for (size_t i = 0; i < sizeof(tx_data); i++) {
        printf("%02X ", tx_data[i]);
    }
    printf("\n");
    
    printf("RX Data: ");
    for (size_t i = 0; i < sizeof(rx_data); i++) {
        printf("%02X ", rx_data[i]);
    }
    printf("\n");
    
    bool loopback_ok = (memcmp(tx_data, rx_data, sizeof(tx_data)) == 0);
    if (loopback_ok) {
        printf("✅ Loopback verification: PASSED\n");
    } else {
        printf("❌ Loopback verification: FAILED\n");
    }
    
    // Step 6: Full-duplex transfer
    printf("\nStep 6️⃣: Full-duplex transfer...\n");
    uint8_t tx_full[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t rx_full[4] = {0};
    ret = alp_spi_transfer(spi, tx_full, rx_full, sizeof(tx_full));
    if (ret != ALP_STATUS_OK) {
        printf("❌ Failed transfer: %d\n", ret);
        alp_spi_deinit(spi);
        alp_spi_destroy(spi);
        return -1;
    }
    
    printf("Transfer TX: ");
    for (size_t i = 0; i < sizeof(tx_full); i++) {
        printf("%02X ", tx_full[i]);
    }
    printf("\n");
    
    printf("Transfer RX: ");
    for (size_t i = 0; i < sizeof(rx_full); i++) {
        printf("%02X ", rx_full[i]);
    }
    printf("\n");
    
    // Step 7: Check status
    printf("\nStep 7️⃣: Checking SPI status...\n");
    alp_spi_status_t status;
    ret = alp_spi_get_status(spi, &status);
    if (ret == ALP_STATUS_OK) {
        printf("  - Busy: %s\n", status.busy ? "Yes" : "No");
        printf("  - Data lost: %s\n", status.data_lost ? "Yes" : "No");
        printf("  - Mode fault: %s\n", status.mode_fault ? "Yes" : "No");
    }
    
    bool is_busy = alp_spi_is_busy(spi);
    printf("  - Is busy (direct): %s\n", is_busy ? "Yes" : "No");
    
    // Step 8: Cleanup
    printf("\nStep 8️⃣: Cleanup...\n");
    ret = alp_spi_deinit(spi);
    if (ret != ALP_STATUS_OK) {
        printf("⚠️  Warning: Deinit returned: %d\n", ret);
    }
    
    ret = alp_spi_destroy(spi);
    if (ret != ALP_STATUS_OK) {
        printf("⚠️  Warning: Destroy returned: %d\n", ret);
    }
    
    // Summary
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════╗\n");
    printf("║                      ✅ SUCCESS!                           ║\n");
    printf("║                                                            ║\n");
    printf("║  The ALP SDK API works perfectly WITHOUT vendor SDK!      ║\n");
    printf("║                                                            ║\n");
    printf("║  Next Steps:                                               ║\n");
    printf("║  1. Try with real hardware (Alif driver)                  ║\n");
    printf("║  2. Add your own platform drivers                         ║\n");
    printf("║  3. Build amazing embedded applications!                  ║\n");
    printf("╚════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    return 0;
}
