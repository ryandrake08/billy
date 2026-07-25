// Application layer: the fish's top-level state machine.
#pragma once

typedef enum
{
    FISH_IDLE,      // asleep, waiting for an activation event
    FISH_ACTIVATE,  // woken; cue that we're listening
    FISH_LISTEN,    // capture the user's utterance
    FISH_THINK,     // transcribe, decide whether there's anything to answer
    FISH_SPEAK,     // stream the reply and voice it with mouth sync
} fish_state_t;

// Spawn the turn loop (IDLE -> ACTIVATE -> LISTEN -> THINK -> SPEAK -> IDLE, driving the HAL and
// transport layers) as its own FreeRTOS task, then return. The loop runs forever.
void runloop_start(void);
