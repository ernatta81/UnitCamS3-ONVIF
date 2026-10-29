/**
 * @file rtsp_server.h
 * @brief Minimal RTSP server streaming the camera as MJPEG over RTP (RFC 2326, RFC 2435)
 * @version 0.1
 * @date 2026-10-29
 *
 */
#pragma once
#include <Arduino.h>

namespace RtspServer
{
    // Starts the server task. Any url path is accepted, APP_RTSP_PATH is what gets advertised.
    // An empty password disables authentication.
    void start(uint16_t port, int fps, const String& user, const String& password);

    // Number of clients currently receiving the stream
    int get_client_count();
}
