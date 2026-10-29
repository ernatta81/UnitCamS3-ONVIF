/**
 * @file hal_config.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include "hal.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <FS.h>


using namespace HAL;


static const char* _config_file_path = "/config.json";


// Keys missing from the file keep their default value
template <typename T>
static void _copy_if_set(JsonDocument& doc, const char* key, T& value)
{
    if (!doc[key].isNull())
        value = doc[key].as<T>();
}


bool hal::saveConfig()
{
    // Try open
    File file = LittleFS.open(_config_file_path, "w");
    if (!file)
    {
        log_e("open %s failed", _config_file_path);
        return false;
    }

    DynamicJsonDocument doc(1024);

    doc["wifiSsid"] = _config.wifi_ssid;
    doc["wifiPass"] = _config.wifi_password;
    doc["hostname"] = _config.hostname;
    doc["frameSize"] = _config.frame_size;
    doc["jpegQuality"] = _config.jpeg_quality;
    doc["fps"] = _config.fps;
    doc["vflip"] = _config.vflip;
    doc["hmirror"] = _config.hmirror;
    doc["authUser"] = _config.auth_user;
    doc["authPass"] = _config.auth_password;

    serializeJson(doc, file);

    file.close();
    return true;
}


void hal::_config_init()
{
    // File system
    if (!LittleFS.begin(true))
    {
        log_e("file system init failed, use default config");
        return;
    }

    log_i("config init");

    // Check exist
    File file = LittleFS.open(_config_file_path, "r");
    if (!file)
    {
        log_i("%s not exist, create default", _config_file_path);
        saveConfig();
        printConfig();
        return;
    }

    // Parse json
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error)
    {
        log_e("parse %s failed: %s, use default config", _config_file_path, error.c_str());
        return;
    }

    _copy_if_set(doc, "wifiSsid", _config.wifi_ssid);
    _copy_if_set(doc, "wifiPass", _config.wifi_password);
    _copy_if_set(doc, "hostname", _config.hostname);
    _copy_if_set(doc, "frameSize", _config.frame_size);
    _copy_if_set(doc, "jpegQuality", _config.jpeg_quality);
    _copy_if_set(doc, "fps", _config.fps);
    _copy_if_set(doc, "vflip", _config.vflip);
    _copy_if_set(doc, "hmirror", _config.hmirror);
    _copy_if_set(doc, "authUser", _config.auth_user);
    _copy_if_set(doc, "authPass", _config.auth_password);

    // Sanitize hand written files
    if (!isValidFrameSize(_config.frame_size))
        _config.frame_size = "VGA";
    _config.jpeg_quality = constrain(_config.jpeg_quality, 6, 63);
    _config.fps = constrain(_config.fps, 1, 30);
    if (_config.hostname == "")
        _config.hostname = "unitcams3";
    if (_config.auth_user == "")
        _config.auth_user = "admin";

    printConfig();
}


void hal::printConfig()
{
    printf("config:\n");
    printf("wifi ssid: %s\n", _config.wifi_ssid.c_str());
    printf("hostname: %s\n", _config.hostname.c_str());
    printf("frame size: %s\n", _config.frame_size.c_str());
    printf("jpeg quality: %d\n", _config.jpeg_quality);
    printf("fps: %d\n", _config.fps);
    printf("vflip: %d hmirror: %d\n", _config.vflip, _config.hmirror);
    printf("auth: %s\n", isAuthEnabled() ? _config.auth_user.c_str() : "disabled");
}
