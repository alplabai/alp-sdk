/**
 ********************************************************************************
 * @file    main.c
 * @brief   ALP SDK CMSIS Example for Alif Ensemble
 * @note    This example shows ALP SDK integration in CMSIS-based Alif project
 ********************************************************************************
 */

#include "alp_spi_vft.h"
#include <stdio.h>

int main(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("  ALP SDK - Alif CMSIS Integration Example\n");
    printf("==================================================\n\n");
    
    // Create Alif SPI handle (uses CMSIS Driver_SPI0 under the hood)
    alp_spi_handle_t *spi = alp_spi_create_alif(0);
    if (!spi) {
        printf("ERROR: Failed to create SPI handle\n");
        return -1;
    }
    
    // Configure SPI
    alp_spi_config_t config = {
        .instance = 0,
        .mode = ALP_SPI_MODE_MASTER,
        .clock_mode = ALP_SPI_CPOL0_CPHA0,
        .bit_order = ALP_SPI_MSB_FIRST,
        .ss_mode = ALP_SPI_SS_MASTER_HW_OUTPUT,
        .data_bits = 8,
        .baudrate = 1000000  // 1 MHz
    };
    
    alp_status_t status = alp_spi_init(spi, &config, NULL);
    if (status != ALP_STATUS_OK) {
        printf("ERROR: SPI init failed\n");
        return -1;
    }
    
    printf("SUCCESS: ALP SDK initialized on Alif hardware!\n");
    printf("  Platform: Alif Ensemble (CMSIS Driver)\n");
    printf("  SPI Instance: 0\n");
    printf("  Baudrate: %u Hz\n\n", config.baudrate);
    
    // Send test data
    uint8_t tx_data[] = {0x01, 0x02, 0x03, 0x04};
    status = alp_spi_send(spi, tx_data, sizeof(tx_data));
    
    if (status == ALP_STATUS_OK) {
        printf("Sent %zu bytes via ALP API\n", sizeof(tx_data));
        printf("  TX: ");
        for (size_t i = 0; i < sizeof(tx_data); i++) {
            printf("%02X ", tx_data[i]);
        }
        printf("\n");
    }
    
    // Cleanup
    alp_spi_deinit(spi);
    free(spi);
    
    printf("\n==================================================\n");
    printf("  ALP SDK working on Alif!\n");
    printf("==================================================\n\n");
    
    return 0;
}
