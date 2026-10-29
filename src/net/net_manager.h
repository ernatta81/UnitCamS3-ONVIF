/**
 * @file net_manager.h
 * @brief Wifi station with a fallback setup access point
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

namespace NetManager
{
    // Connect to the configured wifi, start the setup access point if that is not possible
    void begin();
    // Call from loop()
    void update();

    bool is_setup_mode();
    IPAddress get_local_ip();

    String get_rtsp_url();
    String get_snapshot_url();
    String get_mjpeg_url();
    String get_onvif_url();
}
