// Pin map for the Billy fish. Dev board is an ESP32-S3-WROOM-1 N8R8 (octal PSRAM); the
// board/billy main board (Rev.0/1) populates the N4R2 (quad PSRAM) variant.
// Values are GPIO numbers. Pin-selection rules are already honored: ADC1 for the photocell
// (ADC2 is unusable while WiFi is on), an RTC-capable GPIO for the deep-sleep-wake button, and
// no strapping/flash/USB pins used anywhere.
#pragma once

// 0 = original dev/bench build: both DRV8833s' nSLEEP tied together and both nFAULT tied
//     together (one GPIO each) -- no independent activation or per-chip fault detection.
// 1 = board/billy production PCB: each DRV8833 gets its own nSLEEP/nFAULT.
#define BOARD_REV 0

// Unwired board: For testing, define this to make the software testable even without
// things like the photocell and mic connected
#define BOARD_UNWIRED 0

// --- Amplifier: MAX98357A, I2S0 TX (playback) ---
#define BOARD_AMP_I2S_LRCLK    7
#define BOARD_AMP_I2S_BCLK    15
#define BOARD_AMP_I2S_DIN     16
#define BOARD_AMP_SD_MODE     17  // drive low to mute / power-down the amp (pops, deep sleep)

// --- Microphone: ICS-43434 / INMP441, I2S1 RX (capture) ---
#define BOARD_MIC_I2S_SCK      4
#define BOARD_MIC_I2S_WS       6
#define BOARD_MIC_I2S_SD       5  // mic L/R pin tied to GND -> left channel

// --- Motor drivers: 2x DRV8833 (three spring-return motors) ---
// Each motor is driven one direction only: IN1 = PWM, IN2 held low (the spring returns it).
// IN2 stays on a GPIO so reverse remains a firmware option if a motor is found reversible.
// drv1 drives mouth (AIN) and head (BIN); drv2 drives tail alone -- so nSLEEP/nFAULT are always
// grouped mouth+head vs. tail below. On rev.0 the two chips' nSLEEP are tied together and their
// open-drain nFAULT wire-OR'd together (one GPIO each, so both groups resolve to the same pin
// here); rev.1 gives each chip its own pair.
#define BOARD_MOUTH_IN1       10  // drv1 AIN1, PWM (LEDC): mouth open amount for lip-sync
#define BOARD_MOUTH_IN2       11  // drv1 AIN2, held low (mouth unidirectional; spring return)
#define BOARD_HEAD_IN1        12  // drv1 BIN1, PWM: head raise
#define BOARD_HEAD_IN2        13  // drv1 BIN2, held low (head unidirectional; spring return)
#define BOARD_TAIL_IN1        21  // drv2 AIN1, PWM: tail flap
#define BOARD_TAIL_IN2        47  // drv2 AIN2, held low (tail unidirectional; spring return)

#if BOARD_REV == 0
#define BOARD_MOUTH_HEAD_NSLEEP   14  // drv1+drv2 shared nSLEEP: high = enabled; low in deep sleep -> ~uA
#define BOARD_MOUTH_HEAD_NFAULT   48  // drv1+drv2 shared nFAULT, wire-OR'd (open-drain + pull-up): low = OCP/thermal/UVLO on either chip
#define BOARD_TAIL_NSLEEP         14  // same physical pin as BOARD_MOUTH_HEAD_NSLEEP -- rev.0 can't separate them
#define BOARD_TAIL_NFAULT         48  // same physical pin as BOARD_MOUTH_HEAD_NFAULT -- rev.0 can't separate them
#elif BOARD_REV == 1
#define BOARD_MOUTH_HEAD_NSLEEP   42  // drv1 nSLEEP: high = enabled; drive low in deep sleep -> ~uA
#define BOARD_MOUTH_HEAD_NFAULT   41  // drv1 nFAULT, open-drain + pull-up: low = OCP/thermal/UVLO
#define BOARD_TAIL_NSLEEP         40  // drv2 nSLEEP
#define BOARD_TAIL_NFAULT         39  // drv2 nFAULT, open-drain + pull-up
#else
#error "Unsupported BOARD_REV"
#endif

// --- Inputs ---
#define BOARD_BUTTON           2  // active-low + pull-up; has an EXTERNAL ~10k pull-up
#define BOARD_MODE_SW          1  // stock ON-ON DPDT, 2-position, one pole used: button-wake (floating/high) vs wakeword-wake (grounded/low); has an EXTERNAL ~10k pull-up
#define BOARD_PHOTOCELL_ADC    9  // ADC1 (ADC2 is unusable while WiFi is on)

// --- Status LED: WS2812-family single RGB pixel. Bench: DevKitC-1's onboard WS2812 (DIN only,
// VDD unswitched). Final board: a single WS2812B with VDD gated by BOARD_PERIPHERALS_EN
#define BOARD_STATUS_LED      38  // data (DIN), through a series resistor

// --- Peripherals enable: shared low-side gate for the status LED plus the photocell and
// Vmotor-sense dividers' GND returns -- high whenever the chip is running in either wake mode,
// low during BUTTON-mode deep sleep
#define BOARD_PERIPHERALS_EN   18

// --- Vmotor sense (J13): ADC1, resistor divider from Vdrive to GND -----------------------------
// Rev.0 boards populate this test-point footprint as a bare scope-probe header instead (150R
// series to the GPIO, 10k pulldown to GND, no divider) -- reads near 0 there, which is harmless.
#define BOARD_VMOTOR_ADC       8  // ADC1_CH7 (ADC2 is unusable while WiFi is on)
