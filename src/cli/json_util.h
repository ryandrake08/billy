// Minimal hand-rolled JSON string extraction/escaping -- avoids a JSON library dependency for
// the handful of flat {"key": "value"} objects this protocol uses.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Extract a top-level JSON string field -- {"key":"value with \"escapes\""} -- into `out`.
// Both backends we talk to emit raw UTF-8, so non-ASCII passes straight through and there's no
// \uXXXX to decode.
bool json_get_string(const char *json, const char *key, char *out, size_t out_len);

// Escape a C string for embedding inside a JSON string literal.
void json_escape(const char *in, char *out, size_t out_len);
