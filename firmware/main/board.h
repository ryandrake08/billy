// Pin map for the Billy fish — ESP32-S3-WROOM-1 N8R8.
// Single source of truth for wiring: the HAL includes this; nothing else hard-codes a GPIO.
// Values are GPIO numbers. Pin-selection rules are already honored: ADC1 for the photocell
// (ADC2 is unusable while WiFi is on), an RTC-capable GPIO for the deep-sleep-wake button, and
// no strapping/flash/USB pins used anywhere.
#pragma once

// --- Amplifier: MAX98357A, I2S0 TX (playback) ---
#define BOARD_AMP_I2S_BCLK    15
#define BOARD_AMP_I2S_LRCLK   16
#define BOARD_AMP_I2S_DIN      7
#define BOARD_AMP_SD_MODE     17  // drive low to mute / power-down the amp (pops, deep sleep)

// --- Microphone: ICS-43434 / INMP441, I2S1 RX (capture) ---
#define BOARD_MIC_I2S_SCK      4
#define BOARD_MIC_I2S_WS       6
#define BOARD_MIC_I2S_SD       5  // mic L/R pin tied to GND -> left channel

// --- Motor drivers: 2x DRV8833 (three spring-return motors) ---
// Each motor is driven one direction only: IN1 = PWM, IN2 held low (the spring returns it).
// IN2 stays on a GPIO so reverse remains a firmware option if a motor is found reversible.
// nSLEEP/nFAULT are shared across both chips (one GPIO each; the two open-drain nFAULT lines
// wire-OR'd onto one input).
#define BOARD_MOUTH_IN1       10  // drv1 AIN1, PWM (LEDC): mouth open amount for lip-sync
#define BOARD_MOUTH_IN2       11  // drv1 AIN2, held low (mouth unidirectional; spring return)
#define BOARD_HEAD_IN1        21  // drv2 AIN1, PWM: head raise
#define BOARD_HEAD_IN2        47  // drv2 AIN2, held low (head unidirectional; spring return)
#define BOARD_TAIL_IN1        12  // drv1 BIN1, PWM: tail flap
#define BOARD_TAIL_IN2        13  // drv1 BIN2, held low (tail unidirectional; spring return)
#define BOARD_DRV_NSLEEP      14  // both chips: high = enabled; drive low in deep sleep -> ~uA
#define BOARD_DRV_NFAULT       8  // both chips wire-OR'd (open-drain + pull-up): low = OCP/thermal/UVLO

// --- Inputs ---
#define BOARD_BUTTON           2  // active-low + pull-up; has an EXTERNAL ~10k pull-up
#define BOARD_MODE_SW          1  // stock ON-ON DPDT, 2-position, one pole used: button-wake (floating/high) vs wakeword-wake (grounded/low); has an EXTERNAL ~10k pull-up
#define BOARD_PHOTOCELL_ADC    9  // ADC1 (ADC2 is unusable while WiFi is on)

// --- Status LED: WS2812-family single RGB pixel. Bench: DevKitC-1's onboard WS2812. Final board:
// a discrete WS2812B (5050 or 2020 package). Same GPIO on the bare WROOM-1 module.
#define BOARD_STATUS_LED      38
