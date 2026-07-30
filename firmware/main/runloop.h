// Application layer: the fish's top-level turn loop.
#pragma once

// Spawn the turn loop (idle -> listen -> transcribe -> speak -> idle, driving the HAL and
// transport layers) as its own FreeRTOS task, then return. The loop runs forever.
void runloop_start(void);
