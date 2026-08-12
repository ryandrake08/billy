// Motor drivers: 2x DRV8833, three unidirectional channels (mouth/head/tail).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// GPIO (IN2 pins, nSLEEP, nFAULT) + one shared LEDC timer and three PWM channels. Call once,
// before any other motors_* function.
void motors_init(void);

// nSLEEP low -- park both DRV8833s (~uA standby).
void motors_park(void);

// BUTTON-mode deep sleep only: holds nSLEEP low through the sleep (motors_park() sets the level;
// this makes it survive the reset.
void motors_hold_for_sleep(void);

// Enables both DRV8833s (nSLEEP high).
void motors_enable(void);

// Mouth lip-sync gate, 0-100% duty -- duty-max arithmetic is handled internally, so callers only
// ever need to think in percent.
void motors_set_mouth_pct(uint8_t pct);

// ACTIVATE: a single "I'm listening" gesture -- drive the tail out and let the spring return it.
// Blocking; requires motors_init() to have already run.
void motors_tail_flap(void);

// SPEAK: raise the head and hold it (non-blocking -- stays driven until motors_head_relax()).
void motors_head_out(void);
void motors_head_relax(void);

// Bench self-test (not in the E2E boot path): one motor at a time, sweeping duty to find the
// minimum duty that overcomes the mechanism's spring preload/gearing -- stops immediately on any
// nFAULT trip. Requires motors_init() to have already run.
void motors_selftest(void);

// Progressive combined-load test: head, then head+tail, then head+tail+mouth, each at 100% duty
// for 2 s, then all off. Measures real combined-load rail sag. Stops immediately on any nFAULT
// trip. Requires motors_init() to have already run.
void motors_stresstest(void);
