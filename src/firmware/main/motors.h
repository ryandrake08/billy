// Motor drivers: 2x DRV8833, three unidirectional channels (mouth/head/tail).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// GPIO (IN2 pins, nSLEEP, nFAULT) + one shared LEDC timer and three PWM channels. Call once,
// before any other motors_* function.
void motors_init(void);

// Clear every PWM command and drive nSLEEP low -- park both DRV8833s (~uA standby).
void motors_park(void);

// BUTTON-mode deep sleep only: holds nSLEEP low through the sleep (motors_park() sets the level;
// this makes it survive the reset.
void motors_hold_for_sleep(void);

// Enables both DRV8833s (nSLEEP high). Logs and returns false while nFAULT is latched.
bool motors_enable(void);

// A falling nFAULT edge immediately lowers nSLEEP and latches until a controlled recovery.
bool motors_faulted(void);

// Activation-boundary recovery: require nFAULT high, wake the drivers with all duties zero,
// wait for the datasheet's 1 ms startup time, and require nFAULT to remain high. Leaves the
// drivers parked whether recovery succeeds or fails.
bool motors_recover(void);

// Mouth lip-sync gate, 0-100% duty -- duty-max arithmetic is handled internally, so callers only
// ever need to think in percent. Logs and returns false if nFAULT is latched.
bool motors_set_mouth_pct(uint8_t pct);

// ACTIVATE: a single "I'm listening" gesture -- drive the tail out and let the spring return it.
// Blocking; logs and returns false if nFAULT prevents or interrupts it. Requires motors_init().
bool motors_tail_flap(void);

// SPEAK: raise the head and hold it (non-blocking -- stays driven until motors_head_relax()).
// Both operations log and return false if nFAULT prevents or interrupts the command.
bool motors_head_out(void);
bool motors_head_relax(void);

// Bench self-test (not in the E2E boot path): one motor at a time, sweeping duty to find the
// minimum duty that overcomes the mechanism's spring preload/gearing -- stops immediately on any
// nFAULT trip. Requires motors_init() to have already run.
void motors_selftest(void);

// Progressive combined-load test: head, then head+tail, then head+tail+mouth, each at 100% duty
// for 2 s, then all off. Measures real combined-load rail sag. Stops immediately on any nFAULT
// trip. Requires motors_init() to have already run.
void motors_stresstest(void);
