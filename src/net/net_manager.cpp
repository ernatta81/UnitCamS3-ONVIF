/**
 * @file net_manager.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include "net_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include "../app_config.h"
#include "../hal/hal.h"


static bool _is_setup_mode = false;
static uint32_t _setup_ap_idle_since = 0;


static bool _connect_station()
{
    auto& config = HAL::hal::GetHal()->getConfig();
    if (config.wifi_ssid == "")
    {
        log_i("no wifi configured");
        return false;
    }

    printf("connecting to %s\n", config.wifi_ssid.c_str());
    WiFi.setHostname(config.hostname.c_str());
    WiFi.mode(WIFI_STA);
    // Modem sleep adds a lot of latency and jitter to the stream
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_password.c_str());

    uint32_t time_count = millis();
    bool led_state = false;
    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - time_count > APP_WIFI_CONNECT_TIMEOUT_MS)
        {
            log_e("connect failed");
            WiFi.disconnect(true);
            return false;
        }

        led_state = !led_state;
        HAL::hal::GetHal()->setLed(led_state);
        delay(250);
    }

    printf("connected, ip: %s\n", WiFi.localIP().toString().c_str());
    return true;
}


static void _start_setup_ap()
{
    // Open access point, like the user demo
    String ap_ssid = "UnitCamS3-" + HAL::hal::GetHal()->getDeviceId().substring(6);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid.c_str());

    _is_setup_mode = true;
    _setup_ap_idle_since = millis();
    printf("setup ap: %s, open http://%s/\n", ap_ssid.c_str(), WiFi.softAPIP().toString().c_str());
}


void NetManager::begin()
{
    if (_connect_station())
    {
        // ONVIF clients want a sane clock
        configTime(0, 0, APP_NTP_SERVER_1, APP_NTP_SERVER_2);
        HAL::hal::GetHal()->setLed(true);
    }
    else
    {
        _start_setup_ap();
    }

    auto& config = HAL::hal::GetHal()->getConfig();
    if (MDNS.begin(config.hostname.c_str()))
    {
        MDNS.addService("http", "tcp", APP_HTTP_PORT);
        MDNS.addService("rtsp", "tcp", APP_RTSP_PORT);
    }
}


void NetManager::update()
{
    if (!_is_setup_mode)
        return;

    // Slow blink while waiting for setup
    HAL::hal::GetHal()->setLed((millis() / 1000) % 2);

    if (WiFi.softAPgetStationNum() > 0)
    {
        _setup_ap_idle_since = millis();
        return;
    }

    // Saved wifi was probably just down at boot, try it again
    if (HAL::hal::GetHal()->getConfig().wifi_ssid != "" && millis() - _setup_ap_idle_since > APP_SETUP_AP_IDLE_REBOOT_MS)
    {
        log_i("setup ap idle, reboot to retry wifi");
        HAL::hal::GetHal()->requestReboot();
    }
}


bool NetManager::is_setup_mode()
{
    return _is_setup_mode;
}


IPAddress NetManager::get_local_ip()
{
    return _is_setup_mode ? WiFi.softAPIP() : WiFi.localIP();
}


String NetManager::get_rtsp_url()
{
    return "rtsp://" + get_local_ip().toString() + ":" + String(APP_RTSP_PORT) + APP_RTSP_PATH;
}


String NetManager::get_snapshot_url()
{
    return "http://" + get_local_ip().toString() + "/api/v1/capture";
}


String NetManager::get_mjpeg_url()
{
    return "http://" + get_local_ip().toString() + "/api/v1/stream";
}


String NetManager::get_onvif_url()
{
    return "http://" + get_local_ip().toString() + "/onvif/device_service";
}
