# XIAO ESP32-S3 Sense — Hands-Free Voice Assistant (Realtime + web_search)

Hands-free voice assistant for the Seeed Studio XIAO ESP32-S3 Sense. The on-device VAD captures an utterance, the OpenAI Realtime API understands the speech, and the model may request a web-assisted answer. When it does, the ESP32 calls the Responses API with the built-in `web_search` tool (`tool_choice: "auto"`), returns the result through the Realtime function-call lifecycle, speaks the final answer live through a PCM5102A DAC as it streams in, prints the matching transcript on Serial, and resumes listening.

No physical button. No custom backend. Spoken output (streamed PCM16 @ 24 kHz through a PCM5102A I2S DAC) plus a Serial transcript.

## Audio output: PCM5102A wiring

A second, independent I2S controller (`I2S_NUM_1`) drives the DAC; the PDM microphone keeps its own `I2S_NUM_0`, so both run concurrently without conflict.

| PCM5102A pin | XIAO ESP32-S3 pin | GPIO |
| --- | --- | --- |
| BCK | D1 | 2 |
| LCK / LRCK | D2 | 3 |
| DIN | D3 | 4 |
| GND | GND | — |
| VIN | 3V3 | — |
| SCK | tie to GND (use the module's internal PLL; no MCLK is driven) | — |

Pins are defined as `kDacBckPin` / `kDacLrckPin` / `kDacDinPin` in [`main.cpp`](src/main.cpp); change them there if your wiring differs. `OPENAI_REALTIME_VOICE` in `secrets.h` selects the Realtime voice (default `marin`).

### If there's no sound (checklist)

Most "I2S looks right but it's silent" cases on PCM5102A breakout boards are one of these, not a firmware bug:

1. **XSMT (soft mute) pin** -- active-high un-mute. On a bare PCM5102A, this must be tied to **3V3**, not left floating. Many breakout boards pull it high on-board via a resistor, but not all -- check your specific board's silkscreen/schematic.
2. **FMT pin** -- format select. Must be tied to **GND** for standard I2S (Philips) format, which is what this firmware configures (`I2S_COMM_FORMAT_STAND_I2S`). If it's floating or high, the DAC may expect left-justified data instead and produce silence or garbled audio.
3. **SCK pin** -- tie to **GND** (see table above) so the chip uses its internal PLL; a floating SCK can prevent the DAC from locking onto BCK/LRCK at all.
4. **VIN** -- most PCM5102A breakout boards want 3V3, not 5V; double-check your specific board.

**Confirmed root cause on the common purple "GY/clone PCM5102A" breakout** (3.5 mm jack, `H1L`/`H2L`/`H3L`/`H4L` solder-jumper block in the middle, dedicated `A3V3` pad next to `FMT`/`XSMT`): `XSMT`, `DEMP`, `FLT`, `FMT` are all tied **LOW by default** via those jumpers (hence the `L` suffix). `FMT=L` is correct (I2S format), but `XSMT=L` means the chip is **muted from the factory** regardless of a perfectly valid I2S signal -- this produced exactly the "clean `i2s_write`, zero audio" symptom. Fix: run a wire from the `XSMT` pad to a solid 3.3 V source (the board's own `A3V3` pad or the XIAO's `3V3` pin both work -- `XSMT` is a high-impedance logic input, so either is fine as long as it's 3.3 V, not 5 V). Leave `FMT`/`DEMP`/`FLT` alone.

**If sound is present but crackly/hissy** ("хрип и треск") once `XSMT` is fixed, check two things in order, since both turned out to matter in practice:

1. **Connection quality on the `XSMT` jumper itself.** A tell-tale sign: the sound improves when you touch the wire/pad with a finger -- that means the connection is high-impedance/floating (loose dupont pin, bad breadboard contact), not a solid 3.3 V, so the chip hovers near its mute threshold and crackles. Fix by soldering the jumper (or at minimum seating/clamping it firmly) rather than resting an unsoldered wire in the through-hole pad.
2. **I2S clock jitter.** At 24 kHz, the standard APB-derived I2S clock divider can't land on an exact bit-clock ratio, which can add enough `BCK` jitter for the PCM5102A to crackle even with correct PCM data. Fixed in firmware via `use_apll = true` in [`audio_out.cpp`](src/audio_out.cpp) (also bumped `dma_buf_count` 6→8 for extra underrun margin).

If crackling persists after both of the above, suspect power-supply/wiring noise: USB power from a PC port is often noisy, and breadboard jumpers for `BCK`/`LRCK`/`DIN`/`GND` can crosstalk with each other -- try a wall/power-bank USB source and the shortest possible direct wires (bypassing the breadboard) as a test, and/or add a 10 µF + 100 nF decoupling capacitor across `VIN`/`GND` right at the module.

### Power-on beep

On every boot, right after the DAC initializes (and before Wi-Fi connects), the firmware plays a short ~150 ms, 880 Hz sine beep (`playPowerOnBeep()` in [`main.cpp`](src/main.cpp)). No spoken greeting, no network dependency -- just an "I'm alive" confirmation for the DAC/I2S path.

### Spoken status cues

After the beep, short pre-rendered PCM phrases (OpenAI TTS `gpt-4o-mini-tts`, voice `marin`, PCM16 mono @ 24 kHz) play through the same DAC path:

| Cue | Phrase | When |
| --- | --- | --- |
| Wi-Fi | "WiFi connected" | After Wi-Fi associates |
| Ready | "You can speak now" | Once after VAD calibration (not on every re-listen) |

Clips live in [`status_cues_data.cpp`](src/status_cues_data.cpp) (generated). To regenerate after changing wording or voice:

```bash
python3 tools/gen_status_cues.py   # from repo root; reads OPENAI_API_KEY from env or src/secrets.h
```

To confirm the firmware side is working independently of the DAC, check Serial for:

- `[audio_out] I2S TX готов: BCK=... LRCK=... DIN=... @ 24000 Hz` at boot (I2S driver installed).
- `[audio_out] первый аудио-чанк получен (... samples)` after the first spoken answer -- confirms `response.output_audio.delta` events are arriving and being decoded. If this line never appears, the problem is in the Realtime session/API side (check for a `[realtime] error: ...` line), not the DAC wiring.
- Any `[audio_out] i2s_write failed (...)` lines -- indicates the I2S driver itself is rejecting writes (bad pin config).

### Streaming behavior

`response.output_audio.delta` events are base64-decoded and pushed into a small PSRAM ring buffer (`src/audio_out.cpp`) as they arrive over the WebSocket; a background FreeRTOS task continuously drains that ring into the DAC. Playback therefore starts within the first audio chunk of the response, not after the full answer has been generated. `askUtterance()` waits for the ring to fully drain before returning, so the firmware doesn't resume listening (and risk picking up the speaker through the mic) mid-sentence.

Speaker loudness is controlled by `AUDIO_OUT_VOLUME` in `secrets.h` (`0.0` = silence, `1.0` = full; see [`secrets.example.h`](src/secrets.example.h)). It scales every sample (power-on beep and Realtime replies) before it reaches the DAC.

Because the Realtime API does not support both `text` and `audio` output modalities at once, the session is configured for `output_modalities: ["audio"]`; the transcript printed on Serial comes from `response.output_audio_transcript.delta`, not a separate text-only response.

## Architecture comparison

The table below reflects the original design decision (text-only output, before a PCM5102A DAC was wired up). The chosen architecture, Approach B, has since been extended to stream spoken audio output instead of text-only -- see "Audio output: PCM5102A wiring" above -- but the underlying transport choice (Realtime WebSocket + ESP32-side Responses/web_search orchestration) is unchanged.

### Approach A — GPT-Live + Responses delegation

| Requirement | Status |
| --- | --- |
| Audio in + text-only out | Not met. Live sessions configure a shared audio format and a voice; the documented WebSocket path streams `session.output_audio.delta`. |
| Reuse local speech detector | Poor fit. Live expects a continuous paced microphone stream and manages turn-taking itself. |
| Model-directed `web_search` | Supported via `delegation.type = "responses"` with `tools: [{type:"web_search"}]` and `tool_choice: "auto"` over `wss://api.openai.com/v1/live/sessions`. |
| ESP32 transport/auth | Bearer API key on WebSocket works, but Live is oriented to server-to-server continuous voice. |

**Verdict:** Approach A can do hosted web search, but it does not cleanly support text-only output or reusing the existing utterance VAD on ESP32-S3.

### Approach B — Realtime WebSocket + ESP32 orchestration (implemented)

| Requirement | Status |
| --- | --- |
| Audio in + streamed audio out | `output_modalities: ["audio"]`; `response.output_audio.delta` chunks are played live through a PCM5102A DAC as they arrive (see "Audio output" above) |
| Reuse local VAD | Server `turn_detection: null`; firmware commits the buffer after `vad::poll()` ends an utterance |
| Model-directed search | Realtime function tool `get_web_assisted_answer`; model chooses with `tool_choice: "auto"` |
| Built-in OpenAI web search | ESP32 calls `POST /v1/responses` with `tools: [{type:"web_search"}]` and `tool_choice: "auto"` |
| No custom backend | Device holds a gitignored project API key (see Authentication) |

**Selected:** Approach B.

## Does this genuinely use model-directed web search?

Yes, in two stages:

1. The Realtime model decides whether to call `get_web_assisted_answer`.
2. The Responses model, given that question, decides whether the hosted `web_search` tool is needed (`tool_choice: "auto"`). Stable facts can be answered without a live search; current news/prices/specs typically trigger search.

Source URLs are preserved from Responses `url_citation` annotations and optional `web_search_call.action.sources`, printed under **Источники**, and also returned inside the function output JSON so the Realtime model can cite them in the final text.

## Existing speech detection (reused)

| Piece | Location |
| --- | --- |
| Speech onset / end | `vad::poll()` in `src/vad_recorder.cpp` |
| Audio format | Capture: PCM16 mono @ 16 kHz; resampled to 24 kHz (API minimum) in small chunks while uploading -- no second full-length buffer |
| Capture task | FreeRTOS task in `src/audio_capture.cpp` |
| Pre-roll | ~320 ms ring buffer flushed when recording starts |
| End of utterance | Silence ≥ `endSilenceMs` after `minUtteranceMs`, or max duration |

While a request is in flight, `audio::pause()` drops frames (capture task keeps running) and `gBusy` prevents starting a second request for the same/next utterance until listening resumes.

## Performance notes (ESP32-S3)

A few device-side optimizations keep latency and memory overhead down, independent of the model/network latency below:

- **Realtime session reuse**: the Realtime WebSocket (and its TLS handshake) is kept open and reused across utterances in `openai_realtime.cpp`; it is only torn down and reconnected after an error, disconnect, or timeout. A fresh TLS+WebSocket handshake otherwise costs hundreds of ms to seconds per utterance.
- **Streamed resample, no second full buffer**: `openai_client.cpp` passes the captured 16 kHz PCM straight through; `openai_realtime.cpp` resamples to the Realtime API's 24 kHz input rate one small chunk at a time while uploading, instead of allocating a second full-length PSRAM copy of the utterance up front.
- **Bulk I/O**: both the Realtime WebSocket RX path and the Responses HTTPS RX path read/concat in blocks rather than one byte/char at a time; outgoing base64 audio chunks are built directly into a pre-reserved buffer instead of growing a `String` one character at a time.
- **Keep-alive HTTPS**: the Responses API connection in `openai_http.cpp` is also reused across calls (`Connection: keep-alive`), with transparent reconnect-and-retry if the kept-alive socket turns out to be stale.

None of this changes the dominant source of end-to-end latency, which is the OpenAI model (and web_search, when used) over the network -- see the latency table below.

## Authentication and TLS

- TLS certificate verification is **enabled** (Google Trust Services roots embedded in `openai_tls.cpp`). `setInsecure()` is not used.
- Realtime WebSocket and Responses both authenticate with `Authorization: Bearer <API_KEY>`.
- OpenAI ephemeral client secrets for browser/WebRTC clients are minted by a server that already has a project key. With **no custom backend**, the safest feasible direct-device option is a **gitignored** project API key in `src/secrets.h`, preferably project-scoped and rotatable.
- Never commit `src/secrets.h`.

## Build / upload / test

```bash
# from the repository root
cp src/secrets.example.h src/secrets.h   # first time only
# edit WIFI_* and OPENAI_API_KEY

~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

Speak naturally after calibration completes.

### Example Serial output — knowledge only (no search)

```
[vad] слышу речь, записываю…
[vad] пауза в речи обнаружена, запись окончена (длительность 2.1с)
Отправляю запись (67244 байт) в OpenAI Realtime…
[realtime] session.created
[realtime] session.updated
[realtime] uploading PCM: 33600 samples @ 16000 Hz -> 50400 samples @ 24000 Hz
[realtime] жду ответ модели…
[realtime] response.done
----- Ответ (расшифровка) -----
DNS translates human-readable domain names into IP addresses so clients can reach hosts on the Internet.
web_search: не нужен
-----------------
Снова слушаю…
```

(The spoken answer is already playing through the DAC by the time `response.done` is logged -- `askUtterance()` waits for `audio_out` to finish draining before `askAudio()` returns, which is when the transcript above gets printed.)

### Example Serial output — with web search

```
[vad] слышу речь, записываю…
[vad] пауза в речи обнаружена, запись окончена (длительность 3.4с)
Отправляю запись (109244 байт) в OpenAI Realtime…
[realtime] модель запросила web-assisted answer (q_len=42)
[responses] POST /v1/responses model=gpt-4.1 …
[realtime] Responses ok used_web_search=1 citations_len=180
[realtime] response.done
----- Ответ (расшифровка) -----
Here are today's major technology headlines: …
web_search: использован
Источники:
Example Tech — https://example.com/tech-news-1
-----------------
Снова слушаю…
```

## Costs, latency, memory

| Item | Notes |
| --- | --- |
| Realtime audio | Billed for both audio input and audio output tokens/time for the Realtime model (streamed audio output is no longer free the way text-only output was). Model id is `OPENAI_REALTIME_MODEL` in `secrets.h` (default `gpt-realtime-mini`; use `gpt-realtime` / `gpt-realtime-2.1` when quality matters more than latency). |
| Responses + web_search | Extra request only when the Realtime model calls the tool; web_search has its own tool call pricing. |
| Typical latency to first sound | A few hundred ms to ~1-2 s after `response.create` for the first `response.output_audio.delta` chunk to arrive and start playing; full answers otherwise follow the same ~2–6 s (no search) / ~5–20 s (web search) model+network latency as before. |
| Peak memory | WAV buffer in PSRAM (~12 s × 16 kHz × 2 ≈ 384 KB + header) is the main PSRAM user -- the 24 kHz resample no longer allocates a second full-length copy. `audio_out`'s playback ring adds ~94 KB PSRAM (1 s of stereo PCM16 @ 24 kHz). WebSocket RX ~4 KB. HTTPS Responses body (JSON parsed in PSRAM) held in a `String` (prefer short answers). Keep free PSRAM > 200 KB headroom. |
| Flash | Firmware plus GTS root PEMs (small). |

## Project layout

```
src/
  main.cpp              State machine: Listening → Sending → Printing
  vad_recorder.*        Existing VAD + pre-roll
  audio_capture.*       PDM mic FreeRTOS capture (I2S_NUM_0, RX)
  audio_out.*           PCM5102A DAC streamed playback (I2S_NUM_1, TX)
  status_cues.*         Pre-rendered spoken status cues (+ generated data)
  openai_client.*       Facade over Realtime ask
  openai_realtime.*     WebSocket session + function-call lifecycle + audio streaming
  openai_responses.*    Responses API + web_search
  openai_http.*         Verified HTTPS helpers
  openai_tls.*          Embedded CA roots
  secrets.example.h     Template (copy to secrets.h)
tools/
  gen_status_cues.py    Regenerate status_cues_data.cpp via OpenAI TTS
```
