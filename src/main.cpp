#include <Arduino.h>
#include <WiFi.h>
#include <cmath>

#include "audio_capture.h"
#include "audio_out.h"
#include "openai_client.h"
#include "secrets.h"
#include "status_cues.h"
#include "status_led.h"
#include "vad_recorder.h"

static constexpr int kPdmClkPin = 42;
static constexpr int kPdmDataPin = 41;

// PCM5102A stereo DAC (I2S TX on I2S_NUM_1, separate from the PDM mic's
// I2S_NUM_0). PCM5102A derives its own clock internally, so no MCLK pin is
// needed; wire SCK on the module to GND per its datasheet.
static constexpr int kDacBckPin = 2;  // D1
static constexpr int kDacLrckPin = 3; // D2
static constexpr int kDacDinPin = 4;  // D3

/** Short power-on beep: confirms the DAC/I2S path is alive without a
 *  spoken greeting or network dependency. */
static void playPowerOnBeep()
{
    constexpr float kFreqHz = 880.0f;
    constexpr size_t kChunkFrames = 480;
    constexpr size_t kTotalFrames = audio_out::kSampleRateHz / 7; // ~150 ms

    static int16_t chunk[kChunkFrames];
    Serial.println("[boot] сигнал включения");

    size_t phase = 0;
    while (phase < kTotalFrames)
    {
        const size_t n = (kTotalFrames - phase < kChunkFrames) ? (kTotalFrames - phase) : kChunkFrames;
        for (size_t i = 0; i < n; i++)
        {
            const float t = static_cast<float>(phase + i) / audio_out::kSampleRateHz;
            chunk[i] = static_cast<int16_t>(8000.0f * sinf(2.0f * static_cast<float>(M_PI) * kFreqHz * t));
        }
        audio_out::enqueuePcm16Mono(chunk, n);
        phase += n;
    }

    audio_out::waitDrained(2000);
}

enum class AppState
{
    Listening,
    Sending,
    Printing,
};

static AppState gState = AppState::Listening;
static bool gBusy = false; // ignore overlapping UtteranceReady while in-flight

static bool connectWiFi()
{
    Serial.print("Подключение к Wi-Fi");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");

        if (millis() - start > 20000)
        {
            Serial.println("\nWi-Fi: таймаут подключения");
            return false;
        }
    }

    Serial.println();
    Serial.print("Wi-Fi подключён, IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("ai-assistant: hands-free Realtime + web_search (Approach B)");

    Serial.printf(
        "PSRAM: найдена=%d, всего=%u байт, свободно=%u байт\n",
        psramFound(),
        static_cast<unsigned>(ESP.getPsramSize()),
        static_cast<unsigned>(ESP.getFreePsram()));

    // User LED is on only while idle and ready to listen; see status_led.h.
    status_led::begin();

    if (!audio_out::begin(kDacBckPin, kDacLrckPin, kDacDinPin, AUDIO_OUT_VOLUME))
    {
        Serial.println("Ошибка инициализации DAC (PCM5102A).");
        while (true)
        {
            delay(1000);
        }
    }

    // Runs before Wi-Fi/mic init so the DAC/I2S path is confirmed in
    // isolation (Wi-Fi radio not yet connected).
    playPowerOnBeep();

    if (!connectWiFi())
    {
        Serial.println("Продолжаю без Wi-Fi — запросы к OpenAI будут отклонены.");
    }
    else
    {
        status_cues::play(status_cues::Cue::WifiConnected);
        audio_out::waitDrained(5000);
    }

    if (!audio::begin(kPdmClkPin, kPdmDataPin))
    {
        Serial.println("Ошибка инициализации микрофона.");
        while (true)
        {
            delay(1000);
        }
    }

    vad::Config vadConfig;
    if (!vad::begin(vadConfig))
    {
        Serial.println("Не хватило PSRAM для буфера записи.");
        while (true)
        {
            delay(1000);
        }
    }

    Serial.printf("PSRAM свободно после буфера записи: %u байт\n", static_cast<unsigned>(ESP.getFreePsram()));

    // Measure the ambient noise floor so VAD thresholds adapt to this
    // specific microphone/room instead of relying on guessed constants.
    vad::calibrate();

    status_cues::play(status_cues::Cue::YouCanSpeakNow);
    audio_out::waitDrained(5000);

    Serial.println("Готов. Говорите — запись начнётся автоматически.");
    status_led::on();
}

void loop()
{
    switch (gState)
    {
    case AppState::Listening:
    {
        const vad::Event event = vad::poll();
        // Ready-to-listen = idle VAD (not recording). Off while speech is
        // being captured, then stays off through Sending/Printing.
        if (vad::isRecording())
        {
            status_led::off();
        }
        else if (!gBusy)
        {
            status_led::on();
        }
        if (event == vad::Event::UtteranceReady && !gBusy)
        {
            status_led::off();
            gBusy = true;
            gState = AppState::Sending;
        }
        break;
    }

    case AppState::Sending:
    {
        status_led::off();

        // Keep the capture task alive but drop frames so a second utterance
        // cannot be queued while this request is in flight.
        audio::pause();

        const size_t wavLen = vad::wavLen();
        Serial.printf(
            "Отправляю запись (%u байт) в OpenAI Realtime… (свободно PSRAM: %u байт)\n",
            static_cast<unsigned>(wavLen),
            static_cast<unsigned>(ESP.getFreePsram()));
        Serial.println("Жду ответ (Realtime + опциональный web_search)…");

        const openai::Config openaiConfig{
            OPENAI_API_KEY,
            OPENAI_REALTIME_MODEL,
            OPENAI_RESPONSES_MODEL,
            OPENAI_SYSTEM_PROMPT,
            OPENAI_REALTIME_VOICE,
            OPENAI_REASONING_EFFORT,
        };
        // Blocks until the spoken answer has finished playing through the
        // PCM5102A DAC (see audio_out::waitDrained() inside askUtterance()).
        const openai::Result result = openai::askAudio(openaiConfig, vad::wavData(), wavLen);

        if (result.ok)
        {
            Serial.println("----- Ответ (расшифровка) -----");
            Serial.println(result.text);
            Serial.printf("web_search: %s\n", result.usedWebSearch ? "использован" : "не нужен");
            if (result.citations.length() > 0)
            {
                Serial.println("Источники:");
                Serial.println(result.citations);
            }
            Serial.println("-----------------");
        }
        else
        {
            Serial.print("Ошибка OpenAI: ");
            Serial.println(result.error);
        }

        gState = AppState::Printing;
        break;
    }

    case AppState::Printing:
    {
        vad::reset();
        audio::resume();
        gBusy = false;
        Serial.println("Снова слушаю…");
        status_led::on();
        gState = AppState::Listening;
        break;
    }
    }
}
