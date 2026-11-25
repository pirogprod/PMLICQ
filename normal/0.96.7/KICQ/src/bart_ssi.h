#pragma once
// OSCAR SSI icon record (item type 0x0014). Independent of Miranda headers.
#include <cstdint>
#include <string>
namespace bartssi {
using Bytes = std::string;
inline void be16(Bytes &b, uint16_t v) { b.push_back(char(v >> 8)); b.push_back(char(v)); }
inline bool iconItem(uint16_t itemId, const Bytes &md5, Bytes &out) {
 if (md5.size() != 16 || !itemId) return false;
 Bytes b;
 // Icon SSI item name is the ASCII avatar ID "1" (not an empty name).
 // TLV 0x00D5 value is EXACTLY 18 bytes: flags (1), hash length (1), MD5 (16).
 // The old writer appended a spurious big-endian word (00 01), causing
 // 20 bytes to follow a declared 18-byte TLV and potentially closing BOS.
 be16(b,1); b.push_back('1'); be16(b,0); be16(b,itemId); be16(b,0x14);
 be16(b,0x0016); be16(b,0x00d5); be16(b,0x0012);
 b.push_back(char(1)); b.push_back(char(16)); b+=md5;
 out.swap(b); return true;
}
inline bool deleteItem(uint16_t itemId, Bytes &out) {
 if (!itemId) return false;
 Bytes b; // The delete record must use the same SSI item name as the add/update.
 be16(b,1); b.push_back('1'); be16(b,0); be16(b,itemId); be16(b,0x14); be16(b,0);
 out.swap(b); return true;
}
} // namespace bartssi
