// Application layer (SCOPING.md §8.1, "app"): the fish's top-level state machine (§6).
#pragma once

typedef enum
{
    FISH_IDLE,      // asleep, waiting for an activation event
    FISH_ACTIVATE,  // woken; cue that we're listening
    FISH_LISTEN,    // capture the user's utterance
    FISH_THINK,     // transcribe, decide whether there's anything to answer
    FISH_SPEAK,     // stream the reply and voice it with mouth sync
} fish_state_t;

// Runs forever: IDLE -> ACTIVATE -> LISTEN -> THINK -> SPEAK -> IDLE, driving the HAL and
// transport layers. Never returns.
void runloop_run(void);
