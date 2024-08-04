#pragma once

#if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(5, 0, 0))
    #ifdef CONFIG_IDF_TARGET_ESP32
            #define ETH_PHY_TYPE_LAN8720        ETH_PHY_LAN8720
        #else
            typedef enum {
                ETH_CLOCK_GPIO0_IN= 0,
                ETH_CLOCK_GPIO16_OUT = 2,
                ETH_CLOCK_GPIO17_OUT = 3
            } eth_clock_mode_t;

            #define ETH_PHY_TYPE_LAN8720 ETH_PHY_MAX
        #endif

    #define ETH_CLK_MODE_LAN8720        ETH_CLOCK_GPIO0_IN

    #define ETH_PHY_ADDR_LAN8720         0
    #define ETH_PHY_MDC_LAN8720         23
    #define ETH_PHY_MDIO_LAN8720        18
    #define ETH_PHY_POWER_LAN8720       -1
    #define ETH_RESET_PIN_LAN8720        1
#endif