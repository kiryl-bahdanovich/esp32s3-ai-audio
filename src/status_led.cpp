#include "status_led.h"

#include <Arduino.h>

namespace status_led
{

namespace
{

/** User LED on the XIAO ESP32-S3 (Sense): GPIO21, active-low. */
constexpr int kLedPin = LED_BUILTIN;

} // namespace

void begin()
{
    pinMode(kLedPin, OUTPUT);
    digitalWrite(kLedPin, HIGH); // active-low: HIGH = off
}

void on()
{
    digitalWrite(kLedPin, LOW); // active-low: LOW = on
}

void off()
{
    digitalWrite(kLedPin, HIGH);
}

} // namespace status_led
