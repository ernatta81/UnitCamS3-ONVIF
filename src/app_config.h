/**
 * @file app_config.h
 * @brief Compile time settings of the network camera firmware
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once

#define APP_MANUFACTURER            "M5Stack"
#define APP_MODEL                   "UnitCamS3"
#define APP_FIRMWARE_VERSION        "1.0.0"

#define APP_HTTP_PORT               80
#define APP_RTSP_PORT               554
#define APP_RTSP_PATH               "/mjpeg/1"

// Station connect timeout before falling back to the setup access point
#define APP_WIFI_CONNECT_TIMEOUT_MS 30000
// Setup access point reboots to retry the saved wifi if nobody uses it for this long
#define APP_SETUP_AP_IDLE_REBOOT_MS (5 * 60 * 1000)

#define APP_NTP_SERVER_1            "pool.ntp.org"
#define APP_NTP_SERVER_2            "time.google.com"
