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
 // SSI item: name length/name (empty), group id 0, item id, type 0x14, TLV length
 be16(b,0); be16(b,0); be16(b,itemId); be16(b,0x14);
 // TLV 0x00d5 = BART ID (type 1, flags 1, hash size 16, MD5)
 be16(b,0x0016); be16(b,0x00d5); be16(b,0x0012);
 be16(b,1); b.push_back(char(1)); b.push_back(char(16)); b+=md5;
 out.swap(b); return true;
}
inline bool deleteItem(uint16_t itemId, Bytes &out) {
 if (!itemId) return false;
 Bytes b; be16(b,0); be16(b,0); be16(b,itemId); be16(b,0x14); be16(b,0);
 out.swap(b); return true;
}
} // namespace bartssi
