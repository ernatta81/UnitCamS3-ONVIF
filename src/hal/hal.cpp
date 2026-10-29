/**
 * @file hal.cpp
 * @brief
 * @version 0.1
 * @date 2026-10-29
 *
 */
#include <Arduino.h>
#include "hal.h"


using namespace HAL;


void hal::init()
{
    Serial.begin(115200);
    log_i("hal init");

    // Led
    gpio_reset_pin(GPIO_NUM_14);
    pinMode(HAL_PIN_LED, OUTPUT);
    setLed(false);

    // Config first, the camera is set up from it
    _config_init();

    // Cam
    _cam_init();
}


void hal::setLed(bool state)
{
    // Active low
    digitalWrite(HAL_PIN_LED, state ? 0 : 1);
}


String hal::getDeviceId()
{
    // Stored lsb first
    uint64_t mac = ESP.getEfuseMac();
    char buffer[13];
    snprintf(buffer, sizeof(buffer), "%02x%02x%02x%02x%02x%02x",
             (uint8_t)(mac), (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
             (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    return buffer;
}


hal* hal::_hal = nullptr;

hal* hal::GetHal()
{
    if (_hal == nullptr)
    {
        _hal = new hal;
    }
    return _hal;
}
