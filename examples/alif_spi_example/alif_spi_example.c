/**
 ********************************************************************************
 * @file    alif_spi_example.c
 * @author  ALP SDK Team
 * @date    22/12/2025
 * @brief   Alif Ensemble SPI Example
 * @note    Requires Alif Ensemble SDK and hardware
 ********************************************************************************
 */

/************************************
 * INCLUDES
 ************************************/
#include "alp_spi_vft.h"
#include <stdio.h>
#include <string.h>

/************************************
 * MAIN
 ************************************/
int main(void)
{
    printf("\n");
    printf("════════════════════════════════════════════════════════\n");
    printf("  ALP SDK - Alif Ensemble SPI Example\n");
    printf("  Platform: Alif E7 (ARM Cortex-M55)\n");
    printf("════════════════════════════════════════════════════════\n\n");
    
    alp_status_t status;
    
    // ========================================================================
    // STEP 1: Create Alif SPI handle
    // ========================================================================
    printf("STEP 1: Creating Alif SPI handle...\n");
    alp_spi_handle_t *spi = alp_spi_create_alif(0);  // SPI0
    if (!spi) {
        printf("❌ Failed to create SPI handle\n");
        return -1;
    }
    printf("✅ SPI handle created\n\n");
    
    // ========================================================================
    // STEP 2: Configure SPI
    // ========================================================================
    printf("STEP 2: Configuring SPI...\n");
    alp_spi_config_t config = {
        .instance = 0,
        .mode = ALP_SPI_MODE_MASTER,
        .clock_mode = ALP_SPI_CPOL0_CPHA0,
        .bit_order = ALP_SPI_MSB_FIRST,
        .ss_mode = ALP_SPI_SS_MASTER_HW_OUTPUT,
        .data_bits = 8,
        .baudrate = 1000000  // 1 MHz
    };
    
    status = alp_spi_init(spi, &config, NULL);
    if (status != ALP_STATUS_OK) {
        printf("❌ Failed to initialize SPI: %d\n", status);
        return -1;
    }
    printf("✅ SPI initialized\n\n");
    
    // ========================================================================
    // STEP 3: Send data to SPI device
    // ========================================================================
    printf("STEP 3: Sending data...\n");
    uint8_t tx_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0xAA, 0xBB, 0xCC};
    printf("TX: ");
    for (uint32_t i = 0; i < sizeof(tx_data); i++) {
        printf("%02X ", tx_data[i]);
    }
    printf("\n");
    
    status = alp_spi_send(spi, tx_data, sizeof(tx_data));
    if (status != ALP_STATUS_OK) {
        printf("❌ Send failed: %d\n", status);
    } else {
        printf("✅ Sent %zu bytes\n", sizeof(tx_data));
    }
    printf("\n");
    
    // ========================================================================
    // STEP 4: Receive data from SPI device
    // ========================================================================
    printf("STEP 4: Receiving data...\n");
    uint8_t rx_data[4] = {0};
    status = alp_spi_receive(spi, rx_data, sizeof(rx_data));
    if (status != ALP_STATUS_OK) {
        printf("❌ Receive failed: %d\n", status);
    } else {
        printf("RX: ");
        for (uint32_t i = 0; i < sizeof(rx_data); i++) {
            printf("%02X ", rx_data[i]);
        }
        printf("\n");
        printf("✅ Received %zu bytes\n", sizeof(rx_data));
    }
    printf("\n");
    
    // ========================================================================
    // STEP 5: Full-duplex transfer
    // ========================================================================
    printf("STEP 5: Full-duplex transfer...\n");
    uint8_t tx_transfer[] = {0x11, 0x22, 0x33, 0x44};
    uint8_t rx_transfer[sizeof(tx_transfer)] = {0};
    
    printf("TX: ");
    for (uint32_t i = 0; i < sizeof(tx_transfer); i++) {
        printf("%02X ", tx_transfer[i]);
    }
    printf("\n");
    
    status = alp_spi_transfer(spi, tx_transfer, rx_transfer, sizeof(tx_transfer));
    if (status != ALP_STATUS_OK) {
        printf("❌ Transfer failed: %d\n", status);
    } else {
        printf("RX: ");
        for (uint32_t i = 0; i < sizeof(rx_transfer); i++) {
            printf("%02X ", rx_transfer[i]);
        }
        printf("\n");
        printf("✅ Transfer complete\n");
    }
    printf("\n");
    
    // ========================================================================
    // STEP 6: Check status
    // ========================================================================
    printf("STEP 6: Checking status...\n");
    alp_spi_status_t spi_status;
    status = alp_spi_get_status(spi, &spi_status);
    if (status == ALP_STATUS_OK) {
        printf("  Busy: %d\n", spi_status.busy);
        printf("  Data lost: %d\n", spi_status.data_lost);
        printf("  Mode fault: %d\n", spi_status.mode_fault);
        printf("✅ Status retrieved\n");
    }
    printf("\n");
    
    // ========================================================================
    // STEP 7: Cleanup
    // ========================================================================
    printf("STEP 7: Cleanup...\n");
    status = alp_spi_deinit(spi);
    if (status != ALP_STATUS_OK) {
        printf("❌ Deinit failed: %d\n", status);
        return -1;
    }
    free(spi);
    printf("✅ SPI deinitialized\n\n");
    
    // ========================================================================
    // DONE
    // ========================================================================
    printf("════════════════════════════════════════════════════════\n");
    printf("  ✅ Alif SPI Example Complete!\n");
    printf("════════════════════════════════════════════════════════\n\n");
    
    return 0;
}
