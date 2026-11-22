#pragma once
// Validate a received OFT2 response against the original prompt.
// The cookie and file metadata must not change during the handshake.
#include "oft2_codec.h"
namespace oft2 {
static inline bool validate_reply(const uint8_t *prompt, const uint8_t *reply,
                                  uint16_t expected_type) {
 if (!prompt || !reply || (expected_type != 0x0202 && expected_type != 0x0204)) return false;
 Header a{}, b{};
 if (!decode(prompt, HEADER_SIZE, a) || !decode(reply, HEADER_SIZE, b)) return false;
 if (a.type != 0x0101 || b.type != expected_type) return false;
 // Ignore only the two-byte packet type; everything else is echoed by this transport.
 return memcmp(prompt, reply, 6) == 0 &&
        memcmp(prompt + 8, reply + 8, HEADER_SIZE - 8) == 0;
}
}
