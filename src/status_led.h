#pragma once

/**
 * Ready-to-listen indicator: the onboard User LED (GPIO21 / LED_BUILTIN on
 * the XIAO ESP32-S3, active-low) is on only while the assistant is idle and
 * waiting for speech. It is off during setup, recording, OpenAI requests,
 * and printing.
 */
namespace status_led
{

/** Configures the LED pin and turns it off. Call once from setup(). */
void begin();

/** Turn the LED on (active-low: pin LOW). */
void on();

/** Turn the LED off (active-low: pin HIGH). */
void off();

} // namespace status_led
