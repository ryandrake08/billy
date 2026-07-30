// Hardware-abstraction layer. Everything the app runloop needs from the physical
// fish, behind a stable interface. Audio I/O (I²S mic + amp), activation (mode switch + button,
// bench-wired as bare jumpers; WAKEWORD mode runs a real detector — see components/wakeword),
// and the motor drivers (2x DRV8833, LEDC PWM) are real; awaiting the bench motors to verify.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// A block of mono 16-bit PCM. `samples` is heap_caps-allocated in PSRAM; free with
// audio_buf_free(). A zeroed buffer (samples == NULL, count == 0) means "no audio".
typedef struct
{
    int16_t *samples;
    size_t   count;        // number of samples
    int      sample_rate;  // Hz
} audio_buf_t;

// Release a buffer's PSRAM samples and zero it. Safe on an already-empty buffer.
void audio_buf_free(audio_buf_t *buf);

// Bring up the hardware: amp SD_MODE high + both I²S controllers running continuously (mic RX,
// amp TX).
void fish_hal_init(void);

// Status colors for the WS2812 status LED (BOARD_STATUS_LED) — a permanent diagnostic indicator,
// present on the final board too (WIRING.md §1/§9.5), not just a bench aid. fish_hal_status_init()
// is standalone (not part of fish_hal_init()) so it can run as the very first thing in app_main()
// and show FISH_STATUS_BOOT immediately at power-on.
typedef enum
{
    FISH_STATUS_BOOT,       // yellow — power-on / booting, before the turn loop starts
    FISH_STATUS_WIFI_WAIT,  // white  — WiFi joined; waiting on the backend to become reachable
    FISH_STATUS_IDLE,       // green  — waiting for a wake event (button or wake word)
    FISH_STATUS_LISTEN,     // blue   — activated, capturing the utterance
    FISH_STATUS_THINK,      // cyan   — transcribing / waiting on the backend's reply
    FISH_STATUS_SPEAK,      // violet — voicing the reply
    FISH_STATUS_ERROR,      // red    — fatal init failure
} fish_status_t;

void fish_hal_status_init(void);
void fish_hal_set_status(fish_status_t status);

// Bench audio self-test (not in the E2E boot path): 440 Hz tone, then a live mic-level log.
void fish_hal_selftest(void);

// Bench motor self-test (not in the E2E boot path, currently unused by main.c): one motor at a
// time, sweeping duty (20/35/50/65/80/100%, 2 s each) to find the minimum duty that overcomes
// the mechanism's spring preload/gearing -- stopping immediately on any nFAULT trip. No forced
// stall -- see WIRING.md §6.2 (friction-fit motor shafts make a deliberate stall risky to the
// gears). Requires fish_hal_init() to have already run.
void fish_hal_motor_selftest(void);

// Progressive combined-motor load test: head, then head+tail, then head+tail+mouth, each at
// 100% duty for 2 s, then all off. Measures real combined-load rail sag (watch a scope/DMM on
// the motor rail while it runs) rather than extrapolating from single-motor data. Stops
// immediately on any nFAULT trip. Requires fish_hal_init() to have already run.
void fish_hal_motor_stresstest(void);

// A short "ready — start talking" beep on the amp.
void fish_hal_prompt_tone(void);

// A brief dissonant alert on the amp -- two clashing frequencies, distinct from the mellow
// prompt/self-test tones -- for a backend/network failure (STT or the shim brain-hop).
void fish_hal_error_tone(void);

// Raw photocell ADC reading (12-bit, 0-4095 over the 0-3.3V range via 12 dB attenuation). No
// consumer yet -- kept warm for a future hook (novelty wake / presence / ambient light,
// SCOPING.md §4) rather than left to fish_hal_init()'s one-shot boot log. Requires
// fish_hal_init() to have already run.
int fish_hal_read_photocell(void);

// IDLE: park the motors. In BUTTON mode this mutes the amp and enters real deep sleep on
// BOARD_BUTTON -- it does not return; the chip fully resets and re-runs app_main() on wake. In
// WAKEWORD mode it returns normally (the mic must stay live for detection, so there's no sleep).
void fish_hal_prepare_sleep(void);

// Why this boot exists: a deep-sleep reboot caused by the button or the mode switch, or no such
// cause at all (a cold boot/flash/reset).
typedef enum
{
    FISH_BOOT_NONE,        // cold boot/flash/reset, or a button reboot rejected by the photocell
    FISH_BOOT_BUTTON,      // rebooted by a (photocell-confirmed) button press
    FISH_BOOT_MODE_CHANGE, // rebooted because the mode switch flipped while asleep
} fish_boot_cause_t;

// The reason this boot exists -- see fish_boot_cause_t. The runloop uses FISH_BOOT_BUTTON to skip
// straight to ACTIVATE instead of re-entering IDLE (which would otherwise immediately call
// fish_hal_prepare_sleep() again and go right back to sleep without ever using the press that
// caused it). FISH_BOOT_MODE_CHANGE and FISH_BOOT_NONE both re-enter IDLE, which puts the chip
// right back to sleep unless the switch is now in WAKEWORD mode.
fish_boot_cause_t fish_hal_boot_cause(void);

// Block until an activation event (button press, or wake word in always-on mode). Returns true
// for a real activation. Returns false if the mode switch flipped while waiting -- the caller
// should re-enter IDLE (re-running fish_hal_prepare_sleep()) so the new mode gets a real shot at
// it, rather than continuing to poll in the old mode's style.
bool fish_hal_wait_for_wake(void);

// Body choreography: a tail flap signals "I'm listening"; the head lifts to speak and
// relaxes when the response completes.
void fish_hal_tail_flap(void);
void fish_hal_head_out(void);
void fish_hal_head_relax(void);

// LISTEN: energy-VAD capture — wait for speech onset, capture until ~0.8 s of silence (or a
// hard cap). Allocates out->samples in PSRAM; the caller frees it with audio_buf_free(). Returns
// ESP_ERR_NO_MEM if the PSRAM allocation fails -- a fatal condition, not worth retrying, since a
// single ~10 s mono 16-bit buffer failing to allocate points at PSRAM exhaustion/corruption
// rather than transient pressure. Otherwise returns ESP_OK; a capture with no real speech (just
// noise/clicks) is a normal outcome, not an error, and comes back as a zeroed *out
// (audio_buf_t's own "no audio" contract) for the caller to skip STT on.
esp_err_t fish_hal_capture_utterance(audio_buf_t *out);

// SPEAK: play one mono PCM chunk at its own sample rate (mouth-motor sync comes later).
void fish_hal_play_with_mouth(const audio_buf_t *audio);
