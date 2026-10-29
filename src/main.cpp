//         _.-'-``-._           ______  ______   ______   ______  _______ _______  ______  81
//       ,'::::::''  `.        | |     | |  | \ | |  \ \ | |  | |   | |     | |   | |  | |  
//      ::::::'        :       | |---- | |__| | | |  | | | |__| |   | |     | |   | |__| |   
//      |:::'          |       |_|____ |_|  \_\ |_|  |_| |_|  |_|   |_|     |_|   |_|  |_|   
//      ;:::  .  ,     :        
//     :-------::-------:       
//     `.__O__.;:.__O__.'       
//     (.:::::(:_)_____.)

/**
 * @file main.cpp
 * @brief M5Stack UnitCamS3 as a network camera: RTSP (MJPEG over RTP) and ONVIF Profile S
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include "app_config.h"
#include "hal/hal.h"
#include "net/net_manager.h"
#include "servers/http_server.h"
#include "rtsp/rtsp_server.h"
#include "onvif/onvif.h"


void setup()
{
    // Init
    HAL::hal::GetHal()->init();

    // Wifi, or the setup access point if it is not configured / reachable
    NetManager::begin();

    // Start servers
    HttpServer::start();
    auto& config = HAL::hal::GetHal()->getConfig();
    RtspServer::start(APP_RTSP_PORT, config.fps, config.auth_user, config.auth_password);
    Onvif::start_discovery();

    printf("web:   http://%s/\n", NetManager::get_local_ip().toString().c_str());
    printf("rtsp:  %s\n", NetManager::get_rtsp_url().c_str());
    printf("onvif: %s\n", NetManager::get_onvif_url().c_str());
}


void loop()
{
    NetManager::update();

    if (HAL::hal::GetHal()->isRebootRequested())
    {
        // Let the http response reach the client first
        delay(1000);
        esp_restart();
    }

    delay(100);
}
