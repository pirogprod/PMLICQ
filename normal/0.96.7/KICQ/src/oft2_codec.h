#pragma once
// Strict OFT2 fixed header validation, independent of socket and UI code.
// Header is 256 bytes; numeric fields use network byte order.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace oft2 {
static const size_t HEADER_SIZE = 256;
static inline uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static inline void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
struct Header {
 uint16_t type = 0;
 uint32_t totalFiles = 0, filesLeft = 0, totalSize = 0, size = 0;
 uint32_t received = 0;
};
static inline bool decode(const uint8_t *p, size_t n, Header &out) {
 if (!p || n < HEADER_SIZE || memcmp(p, "OFT2", 4) || be16(p + 4) != HEADER_SIZE) return false;
 Header h;
 h.type = be16(p + 6);
 h.totalFiles = be16(p + 20);
 h.filesLeft = be16(p + 22);
 const uint16_t totalParts = be16(p + 24), partsLeft = be16(p + 26);
 if (!totalParts || !partsLeft || partsLeft > totalParts) return false;
 h.totalSize = be32(p + 28);
 h.size = be32(p + 32);
 h.received = be32(p + 60);
 if (!h.totalFiles || h.filesLeft > h.totalFiles || h.size > h.totalSize || h.received > h.size) return false;
 out = h;
 return true;
}
}
