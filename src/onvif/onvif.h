/**
 * @file onvif.h
 * @brief Minimal ONVIF Profile S: WS-Discovery plus the device and media services
 *        NVR/VMS clients need to find the camera and its RTSP stream.
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once
#include <Arduino.h>
#include <vector>

class AsyncWebServer;

namespace Onvif
{
    // SOAP endpoints under /onvif/ (device_service, media_service)
    void load_soap_apis(AsyncWebServer& server);

    // WS-Discovery responder on udp 3702, runs in its own task
    void start_discovery();

    // Shared by the soap and discovery code
    String get_device_uuid();
    std::vector<String> get_scopes();
    // Pointer right after the start tag of the first element with this local name (any prefix)
    const char* find_element(const char* xml, const char* local_name);
    String get_element_text(const char* xml, const char* local_name);
}
