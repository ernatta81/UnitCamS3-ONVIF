/**
 * @file hal.h
 * @brief UnitCamS3 board support: camera, status led, persistent config.
 *        Trimmed down from the UnitCamS3 user demo hal.
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once
#include <Arduino.h>


#define HAL_PIN_LED         14


namespace HAL
{
    class hal
    {
    public:
        struct Config_t
        {
            String wifi_ssid;
            String wifi_password;
            String hostname = "unitcams3";
            String frame_size = "VGA";
            int jpeg_quality = 12;
            int fps = 15;
            bool vflip = true;
            bool hmirror = true;
            // Web page, RTSP and ONVIF credentials, empty password = open access
            String auth_user = "admin";
            String auth_password;
        };

    private:
        Config_t _config;
        volatile bool _reboot_requested = false;

        void _cam_init();
        void _config_init();

    public:
        void init();

        void setLed(bool state);

        // Config is stored as json in LittleFS (/config.json)
        inline Config_t& getConfig() { return _config; }
        bool saveConfig();
        void printConfig();

        // Frame size names accepted in the config, e.g. "VGA"
        static bool isValidFrameSize(const String& name);

        inline bool isAuthEnabled() { return _config.auth_password != ""; }

        // Lower case hex efuse mac, e.g. "3485189a1b2c"
        String getDeviceId();

        // Reboot is done from loop() so pending http responses can be flushed first
        inline void requestReboot() { _reboot_requested = true; }
        inline bool isRebootRequested() { return _reboot_requested; }


    private:
        static hal* _hal;
    public:
        static hal* GetHal();
    };
}
