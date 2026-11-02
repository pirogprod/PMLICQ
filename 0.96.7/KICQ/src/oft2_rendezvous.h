#pragma once
// Bounds-checked OSCAR channel-2 file rendezvous TLV parser.
// Parsing only: accepting an offer requires Miranda and OFT2 session handling.
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <array>
namespace oft2 {
struct FileOffer {
 uint16_t kind = 0;
 uint32_t size = 0;
 uint16_t count = 0;
 std::string name;
 bool hasInfo = false;
};
static inline uint16_t rd16(const unsigned char *p) { return (uint16_t(p[0]) << 8) | p[1]; }
static inline uint32_t rd32(const unsigned char *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
// 0x2711: 2-byte file kind, 2-byte file count, 4-byte aggregate size,
// followed by a NUL-terminated filename (legacy OSCAR encoding).
static inline bool parse_file_info(const unsigned char *p, size_t n, FileOffer &out) {
 if (!p || n < 9 || n > 65535) return false;
 FileOffer tmp;
 tmp.kind = rd16(p); tmp.count = rd16(p + 2); tmp.size = rd32(p + 4);
 if (!tmp.count) return false;
 const unsigned char *start = p + 8;
 const unsigned char *end = p + n;
 const unsigned char *nul = start;
 while (nul < end && *nul) ++nul;
 if (nul == end || nul == start || size_t(nul - start) > 255) return false;
 tmp.name.assign((const char *)start, (const char *)nul);
 // Never allow paths from remote peers to select the output directory.
 for (char c : tmp.name) if (c == '/' || c == '\\' || c == ':') return false;
 if (tmp.name == "." || tmp.name == "..") return false;
 tmp.hasInfo = true; out = tmp; return true;
}
static inline bool parse_offer_tlvs(const unsigned char *p, size_t n, FileOffer &out) {
 if (!p) return false;
 size_t offset = 0;
 while (offset < n) {
  if (n - offset < 4) return false;
  uint16_t tag = rd16(p + offset), len = rd16(p + offset + 2);
  offset += 4;
  if (len > n - offset) return false;
  if (tag == 0x2711) return parse_file_info(p + offset, len, out);
  offset += len;
 }
 return false;
}
}

// Construct the channel-2 0x2711 file-info payload. The caller wraps this in
// the OSCAR ICBM rendezvous envelope and supplies its own 8-byte cookie.
namespace oft2 {
static inline bool build_file_info(const FileOffer &offer, std::string &out) {
 if (!offer.count || offer.name.empty() || offer.name.size() > 255) return false;
 if (offer.name == "." || offer.name == "..") return false;
 for (unsigned char c : offer.name)
  if (!c || c == '/' || c == '\\' || c == ':') return false;
 std::string tmp;
 tmp.reserve(9 + offer.name.size());
 tmp.push_back(char(offer.kind >> 8)); tmp.push_back(char(offer.kind));
 tmp.push_back(char(offer.count >> 8)); tmp.push_back(char(offer.count));
 tmp.push_back(char(offer.size >> 24)); tmp.push_back(char(offer.size >> 16));
 tmp.push_back(char(offer.size >> 8)); tmp.push_back(char(offer.size));
 tmp.append(offer.name); tmp.push_back('\0');
 out.swap(tmp); return true;
}
static inline bool build_offer_tlv(const FileOffer &offer, std::string &out) {
 std::string data;
 if (!build_file_info(offer, data)) return false;
 std::string tmp;
 tmp.reserve(data.size() + 4);
 tmp.push_back(char(0x27)); tmp.push_back(char(0x11));
 tmp.push_back(char(data.size() >> 8)); tmp.push_back(char(data.size()));
 tmp += data;
 out.swap(tmp); return true;
}
}

// OSCAR channel-2 rendezvous envelope. The outer ICBM carries an 8-byte
// cookie; TLV 0x0005 contains request type, echoed cookie, 16-byte capability,
// and nested TLVs. This codec does not assume a particular server or client.
namespace oft2 {
struct Rendezvous {
 uint16_t type = 0;
 std::array<unsigned char, 8> cookie{};
 std::array<unsigned char, 16> capability{};
 FileOffer offer;
 bool hasOffer = false;
 uint32_t externalIp = 0;
 uint32_t internalIp = 0;
 uint16_t requestNumber = 0;
 uint16_t port = 0;
};
static inline void append16(std::string &s, uint16_t v) {
 s.push_back(char(v >> 8)); s.push_back(char(v));
}
static inline void append32(std::string &s, uint32_t v) {
 s.push_back(char(v >> 24)); s.push_back(char(v >> 16));
 s.push_back(char(v >> 8)); s.push_back(char(v));
}
static inline bool add_tlv(std::string &out, uint16_t tag, const std::string &value) {
 if (value.size() > 65535 || out.size() + 4 + value.size() > 65535) return false;
 append16(out, tag); append16(out, (uint16_t)value.size()); out += value;
 return true;
}
static inline bool parse_rendezvous(const unsigned char *p, size_t n, Rendezvous &out) {
 if (!p || n < 26 || n > 65535) return false;
 Rendezvous tmp;
 tmp.type = rd16(p);
 if (tmp.type > 2) return false;
 for (size_t i = 0; i < 8; ++i) tmp.cookie[i] = p[i + 2];
 for (size_t i = 0; i < 16; ++i) tmp.capability[i] = p[i + 10];
 size_t pos = 26;
 while (pos < n) {
  if (n - pos < 4) return false;
  uint16_t tag = rd16(p + pos), len = rd16(p + pos + 2);
  pos += 4;
  if (len > n - pos) return false;
  const unsigned char *v = p + pos;
  if (tag == 0x000A) {
   if (len != 2) return false;
   tmp.requestNumber = rd16(v);
  }
  else if (tag == 0x0002) {
   if (len != 4) return false;
   tmp.externalIp = rd32(v);
  }
  else if (tag == 0x2711) {
   if (tmp.hasOffer || !parse_file_info(v, len, tmp.offer)) return false;
   tmp.hasOffer = true;
  }
  else if (tag == 0x0003 || tag == 0x0004) {
   if (len != 4) return false;
   if (tag == 0x0003) tmp.internalIp = rd32(v);
   // 0x0004 is server-verified IP; do not use it as the advertised address.
  }
  else if (tag == 0x0005) {
   if (len != 2) return false;
   tmp.port = rd16(v);
  }
  pos += len;
 }
 out = tmp; return true;
}
static inline bool build_rendezvous(const Rendezvous &rv, std::string &out) {
 if (rv.type > 2) return false;
 std::string value;
 append16(value, rv.type);
 value.append((const char *)rv.cookie.data(), rv.cookie.size());
 value.append((const char *)rv.capability.data(), rv.capability.size());
 if (rv.requestNumber) { std::string v; append16(v, rv.requestNumber); if (!add_tlv(value, 0x000A, v)) return false; }
 if (rv.externalIp) { std::string v; append32(v, rv.externalIp); if (!add_tlv(value, 0x0002, v)) return false; }
 if (rv.internalIp) { std::string v; append32(v, rv.internalIp); if (!add_tlv(value, 0x0003, v)) return false; }
 if (rv.port) { std::string v; append16(v, rv.port); if (!add_tlv(value, 0x0005, v)) return false; }
 if (rv.hasOffer) { std::string v; if (!build_file_info(rv.offer, v) || !add_tlv(value, 0x2711, v)) return false; }
 out.swap(value); return true;
}
}
