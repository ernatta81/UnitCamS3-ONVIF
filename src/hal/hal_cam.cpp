/**
 * @file hal_cam.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include <esp_camera.h>
#include "hal.h"
#include "camera_pins.h"

using namespace HAL;


struct FrameSizeName_t
{
    const char* name;
    framesize_t size;
};

// RTP/JPEG (RFC 2435) limits width and height to 2040
static const FrameSizeName_t _frame_sizes[] = {
    {"QVGA", FRAMESIZE_QVGA},   // 320x240
    {"CIF", FRAMESIZE_CIF},     // 400x296
    {"HVGA", FRAMESIZE_HVGA},   // 480x320
    {"VGA", FRAMESIZE_VGA},     // 640x480
    {"SVGA", FRAMESIZE_SVGA},   // 800x600
    {"XGA", FRAMESIZE_XGA},     // 1024x768
    {"HD", FRAMESIZE_HD},       // 1280x720
    {"SXGA", FRAMESIZE_SXGA},   // 1280x1024
    {"UXGA", FRAMESIZE_UXGA},   // 1600x1200
};


static framesize_t _get_frame_size(const String& name)
{
    for (auto& i : _frame_sizes)
    {
        if (name == i.name)
            return i.size;
    }
    return FRAMESIZE_VGA;
}


bool hal::isValidFrameSize(const String& name)
{
    for (auto& i : _frame_sizes)
    {
        if (name == i.name)
            return true;
    }
    return false;
}


void hal::_cam_init()
{
    log_i("cam init");

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;

    // Frame buffers are sized for this, so changing it needs a reboot
    config.frame_size = _get_frame_size(_config.frame_size);
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = _config.jpeg_quality;
    // RTSP, the http mjpeg stream and snapshots can each hold a frame at the same time
    config.fb_count = 3;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        printf("Camera init failed with error 0x%x\n", err);

        // Retry
        log_i("reboot..");
        delay(1000);
        esp_restart();
    }

    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, _config.vflip ? 1 : 0);
    s->set_hmirror(s, _config.hmirror ? 1 : 0);

    log_i("cam init ok");
    delay(100);
}
