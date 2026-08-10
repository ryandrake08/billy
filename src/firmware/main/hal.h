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
// heap_caps_free(). A zeroed buffer (samples == NULL, count == 0) means "no audio".
typedef struct
{
    int16_t *samples;
    size_t   count;        // number of samples
    int      sample_rate;  // Hz
} audio_buf_t;

// Signals a local audio-hardware fault (currently: i2s_channel_write failing in the amp TX path),
// as distinct from any ordinary ESP-IDF esp_err_t a network/backend call might also return. Callers
// several layers up (net_respond's SSE loop, the runloop) use this to tell "the fish's own speaker
// is broken" apart from "the backend/network had a hiccup" -- the former makes playing the usual
// error tone pointless (it would just fail the same way), so it needs a distinct signal rather than
// being lumped in with generic failures. 0x8001 sits well outside the range of any ESP-IDF or driver
// error code in use here.
#define FISH_ERR_AUDIO_HW ((esp_err_t) 0x8001)

// Status colors for the WS2812 status LED (BOARD_STATUS_LED)
typedef enum
{
    FISH_STATUS_BOOT,       // white  — power-on / booting, before the turn loop starts
    FISH_STATUS_IDLE,       // green  — waiting for a wake event (button or wake word)
    FISH_STATUS_LISTEN,     // blue   — activated, capturing the utterance
    FISH_STATUS_THINK,      // cyan   — transcribing / waiting on the backend's reply
    FISH_STATUS_SPEAK,      // violet — voicing the reply
    FISH_STATUS_ERROR,      // red    — fatal init failure
} fish_status_t;

// Bring up the hardware: status LED first (so it shows FISH_STATUS_BOOT immediately, before
// anything below it could ESP_ERROR_CHECK-panic), then amp SD_MODE high + both I²S controllers
// running continuously (mic RX, amp TX), motors, and the photocell ADC. Call this first in
// app_main(), before the wake-word model load or WiFi join, which can each take a noticeable
// moment.
void fish_hal_init(void);

void fish_hal_set_status(fish_status_t status);

// Rev.0 debug only: reclaims BOARD_MISC_GPIO from ADC mode and drives it high or low, for the
// bare scope-probe header on that board. Do not call on Rev.1 hardware -- the same pin there is
// BOARD_VMOTOR_ADC, wired to the Vdrive divider, and this will fight it. Safe to call repeatedly;
// each call reconfigures the pad fresh. After calling this, fish_hal_read_vmotor() will return
// stale/invalid readings until fish_hal_init() re-runs (not automatic) -- fine on Rev.0, since
// there's no real divider there for it to read anyway.
void fish_hal_set_misc_gpio(bool level);

// Raw Vmotor (Vdrive) ADC reading (12-bit, 0-4095 over the 0-3.3V range via 12 dB attenuation),
// through the board.h divider. On Rev.0 boards, where this pad is a bare scope-probe header
// instead of the divider, this reads near 0. Requires fish_hal_init() to have already run.
int fish_hal_read_vmotor(void);

// Bench audio self-test (not in the E2E boot path): 440 Hz tone, then a live mic-level log.
void fish_hal_selftest(void);

// Bench motor self-test (not in the E2E boot path, currently unused by main.c): one motor at a
// time, sweeping duty to find the minimum duty that overcomes the mechanism's spring
// preload/gearing -- stopping immediately on any nFAULT trip. Requires fish_hal_init() to
// have already run.
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

// Raw photocell ADC reading (12-bit, 0-4095 over the 0-3.3V range via 12 dB attenuation).
// Currently used to inhibit false positive wakes when ambient light is low. Requires
// fish_hal_init() to have already run.
int fish_hal_read_photocell(void);

// IDLE: park the motors. In BUTTON mode this mutes the amp and enters real deep sleep on
// BOARD_BUTTON -- it does not return; the chip fully resets and re-runs app_main() on wake. In
// WAKEWORD mode it returns normally (the mic must stay live for detection, so there's no sleep).
void fish_hal_prepare_sleep(void);

// Which physical pin caused an EXT1 deep-sleep wake, if any -- a raw hardware fact, unlike
// fish_hal_boot_cause() below, which additionally validates a button pin against the photocell
// (and therefore against fish_config_get(), which may not have its freshest value yet -- see
// fish_hal_boot_cause()'s doc comment). Exists so a caller can act on "was this a button press"
// before that validation runs, without creating a circular dependency on boot_cause's own,
// photocell-dependent result.
typedef enum
{
    FISH_WAKE_PIN_NONE,     // not an EXT1 wake (cold boot/flash/reset)
    FISH_WAKE_PIN_BUTTON,   // BOARD_BUTTON was low at wake
    FISH_WAKE_PIN_MODE_SW,  // BOARD_MODE_SW was low at wake
} fish_wake_pin_t;

fish_wake_pin_t fish_hal_deep_sleep_wake_pin(void);

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
//
// For a button pin, this validates against the photocell (fish_config_get()->
// photocell_wake_threshold) *before* the runloop has had any chance to fetch fresh config -- so
// that check always sees the compiled-in default unless a caller forces a fetch first, which
// means checking fish_hal_deep_sleep_wake_pin() (above) rather than this function, since this
// function's own result depends on the very config a forced fetch would change.
fish_boot_cause_t fish_hal_boot_cause(void);

// Block until an activation event (button press, or wake word in always-on mode) passes the
// photocell brightness gate. A single attempt, not a retry loop -- returns false both when the
// mode switch flips while waiting and when a real wake candidate is rejected as too dark, so the
// caller always re-enters IDLE (re-running fish_hal_prepare_sleep() and net_fetch_config())
// between attempts rather than retrying silently inside this call. Returns true only for an
// activation that's both real and photocell-approved.
bool fish_hal_wait_for_wake(void);

// Body choreography: a tail flap signals "I'm listening"; the head lifts to speak and
// relaxes when the response completes.
void fish_hal_tail_flap(void);
void fish_hal_head_out(void);
void fish_hal_head_relax(void);

// LISTEN: energy-VAD capture — wait for speech onset, capture until ~0.8 s of silence (or a
// hard cap). Allocates out->samples in PSRAM; the caller frees it with heap_caps_free(). Returns
// ESP_ERR_NO_MEM if the PSRAM allocation fails -- a fatal condition, not worth retrying, since a
// single ~10 s mono 16-bit buffer failing to allocate points at PSRAM exhaustion/corruption
// rather than transient pressure. Otherwise returns ESP_OK; a capture with no real speech (just
// noise/clicks), or one abandoned partway through by a button press, is a normal outcome, not an
// error, and comes back as a zeroed *out (audio_buf_t's own "no audio" contract) for the caller
// to skip STT on -- the runloop's existing "heard nothing" path is what sends it back to idle,
// so no separate handling is needed for an interrupted capture.
esp_err_t fish_hal_capture_utterance(audio_buf_t *out);

// SPEAK: play one mono PCM buffer through the amp. Resamples internally to AMP_SAMPLE_RATE if
// `audio` isn't already at that rate (the TX clock is fixed at init time) -- the normal TTS path
// is already at AMP_SAMPLE_RATE and never hits this. `move_mouth` drives the lip-sync motor off
// the audio envelope when true; pass false for playback that shouldn't move the mechanism (e.g.
// repeating a captured utterance back for debugging). Returns FISH_ERR_AUDIO_HW if the amp TX
// write fails, ESP_ERR_NO_MEM if a needed resample allocation fails; ESP_OK otherwise (including
// for empty/null audio, which is a no-op, not a failure).
esp_err_t fish_hal_play(const audio_buf_t *audio, bool move_mouth);
