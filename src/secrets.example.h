#pragma once

// Copy this file to secrets.h and fill in your values (secrets.h is gitignored).
//
//   cp src/secrets.example.h src/secrets.h
//
// Then edit src/secrets.h with your real Wi-Fi and OpenAI credentials and
// flash as usual:
//
//   ~/.platformio/penv/bin/pio run -t upload
//   ~/.platformio/penv/bin/pio device monitor

// Wi-Fi must be 2.4 GHz (the ESP32-S3 radio does not support 5 GHz networks).
const char* WIFI_SSID = "your-wifi-ssid";
const char* WIFI_PASSWORD = "your-wifi-password";

// OpenAI API key. Required for Realtime WebSocket + Responses web_search.
// Create one at https://platform.openai.com/api-keys
//
// Authentication note: OpenAI does not mint device-bound ephemeral credentials
// without a backend. Short-lived client secrets for browsers/WebRTC are created
// by a trusted server that already holds a project API key. With "no custom
// backend" this firmware must hold a project API key in gitignored secrets.h.
// Prefer a restricted key, rotate it if the device is lost, and never commit it.
const char* OPENAI_API_KEY = "sk-...";

// Realtime model for audio-in / audio-out (+ transcript) over WebSocket.
// Prefer a mini model for lower voice latency / cost; switch to full when
// tool-use quality matters more than turnaround:
//   "gpt-realtime-mini"      — fast / cheap (default)
//   "gpt-realtime-2.1-mini"  — newer mini with better noise/turn handling
//   "gpt-realtime"          — full model, slower / higher quality
//   "gpt-realtime-2.1"      — newest full model
const char* OPENAI_REALTIME_MODEL = "gpt-realtime-2.1-mini";

// Realtime output voice. See the Realtime API docs for the current list of
// supported voices (e.g. "marin", "alloy", "verse", ...).
const char* OPENAI_REALTIME_VOICE = "marin";

// Speaker playback volume (PCM5102A DAC). 0.0 = silence, 1.0 = full level
// as received from Realtime / the power-on beep. Values above 1.0 amplify
// and may clip.
const float AUDIO_OUT_VOLUME = 0.1f;

// Responses model used when the Realtime model requests web-assisted answering.
// Must support the hosted web_search tool.
const char* OPENAI_RESPONSES_MODEL = "gpt-6-luna";

// Responses API reasoning.effort (e.g. "low", "medium"). Prefer "low" for
// voice latency. GPT-6 often rejects "none". Set to "" to omit the field.
const char* OPENAI_REASONING_EFFORT = "low";

// Shared instructions for Realtime and Responses.
const char* OPENAI_SYSTEM_PROMPT =
    "You are a concise voice assistant. Answer the user's spoken questions "
    "naturally. Use your existing knowledge for stable facts and explanations. "
    "When a question depends on current events, recent changes, current prices, "
    "recent product specifications, or information you cannot reliably answer "
    "from existing knowledge, use web search via the get_web_assisted_answer "
    "tool. Decide autonomously when a search is necessary. Do not claim to have "
    "searched if you have not. Prefer authoritative and recent sources. "
    "Incorporate the findings into a direct spoken answer. Mention source "
    "names or publishers by voice when web search was used, since there is "
    "no screen to display citations on.";
