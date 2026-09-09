// Motor drivers: 2x DRV8833, three unidirectional channels (mouth/head/tail).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// drv1 drives mouth+head, drv2 drives tail -- these are the two independent nSLEEP/nFAULT
// domains. On rev.0 board.h resolves both groups to the same physical pins, so the two chips
// necessarily activate and fault together there; rev.1 keeps them independent.
typedef enum
{
    MOTORS_GROUP_MOUTH_HEAD = 0,
    MOTORS_GROUP_TAIL       = 1,
    MOTORS_GROUP_COUNT,
} motors_group_t;

// GPIO (IN2 pins, both groups' nSLEEP/nFAULT) + one shared LEDC timer and three PWM channels.
// Call once, before any other motors_* function.
void motors_init(void);

// Clear every PWM command and drive both groups' nSLEEP low -- park both DRV8833s (~uA standby).
void motors_park(void);

// BUTTON-mode deep sleep only: holds both groups' nSLEEP low through the sleep (motors_park()
// sets the level; this makes it survive the reset).
void motors_hold_for_sleep(void);

// Enables the given group's DRV8833 (nSLEEP high). Logs and returns false while that group's
// nFAULT is latched.
bool motors_enable(motors_group_t grp);

// Activation-boundary recovery for one group: no-op if that group's nFAULT isn't latched.
// Otherwise requires nFAULT high, wakes the driver with all its duties zero, waits for the
// datasheet's 1 ms startup time, and requires nFAULT to remain high. Leaves the driver parked
// whether recovery succeeds or fails. A fault on one group never blocks the other.
bool motors_recover_if_faulted(motors_group_t grp);

// Mouth lip-sync gate, 0-100% duty -- duty-max arithmetic is handled internally, so callers only
// ever need to think in percent. Logs and returns false if the mouth+head group's nFAULT is latched.
bool motors_set_mouth_pct(uint8_t pct);

// ACTIVATE: a single "I'm listening" gesture -- drive the tail out and let the spring return it.
// Blocking; logs and returns false if the tail group's nFAULT prevents or interrupts it. Requires
// motors_init().
bool motors_tail_flap(void);

// SPEAK: raise the head and hold it (non-blocking -- stays driven until motors_head_relax()).
// Both operations log and return false if the mouth+head group's nFAULT prevents or interrupts
// the command.
bool motors_head_out(void);
bool motors_head_relax(void);
