#pragma once
// Self-contained FLAP/SNAC framing for an authenticated OSCAR BART connection.
// This is a wire codec, NOT the connection lifecycle or Miranda Avatar API.
#include <cstdint>
#include <string>
#include <utility>

namespace bart {
struct FlapFrame {
 uint8_t channel = 0;
 uint16_t sequence = 0;
 std::string payload;
};
struct SnacFrame {
 uint16_t family = 0, subtype = 0, flags = 0;
 uint32_t requestId = 0;
 std::string payload;
};
inline void wire16(std::string &b, uint16_t v) { b.push_back(char(v >> 8)); b.push_back(char(v)); }
inline void wire32(std::string &b, uint32_t v) { wire16(b, uint16_t(v >> 16)); wire16(b, uint16_t(v)); }
inline uint16_t read16(const std::string &s, size_t p) {
 return uint16_t((uint16_t(uint8_t(s[p])) << 8) | uint8_t(s[p+1]));
}
inline uint32_t read32(const std::string &s, size_t p) {
 return (uint32_t(read16(s,p)) << 16) | read16(s,p+2);
}
inline bool makeSnac(const SnacFrame &s, std::string &result) {
 if (s.payload.size() > 65525) return false;
 std::string b;
 wire16(b,s.family); wire16(b,s.subtype); wire16(b,s.flags);
 wire32(b,s.requestId); b += s.payload;
 result.swap(b); return true;
}
inline bool parseSnac(const std::string &data, SnacFrame &result) {
 if (data.size()<10) return false;
 SnacFrame s;
 s.family=read16(data,0); s.subtype=read16(data,2); s.flags=read16(data,4);
 s.requestId=read32(data,6); s.payload=data.substr(10);
 result=std::move(s); return true;
}
inline bool makeFlap(const FlapFrame &f, std::string &result) {
 if(f.payload.size()>65535 || f.channel==0) return false;
 std::string b; b.push_back('*');b.push_back(char(f.channel));wire16(b,f.sequence);
 wire16(b,uint16_t(f.payload.size()));b+=f.payload;
 result.swap(b);return true;
}
// Return 0 if more bytes are needed, -1 if invalid, 1 on success; consume only one frame.
inline int consumeFlap(std::string &buffer, FlapFrame &result) {
 if(buffer.size()<6) return 0;
 if(buffer[0]!='*' || buffer[1]==0) return -1;
 const auto n=read16(buffer,4);
 if(buffer.size()<6u+n) return 0;
 FlapFrame frame;
 frame.channel=uint8_t(buffer[1]);frame.sequence=read16(buffer,2);
 frame.payload=buffer.substr(6,n);buffer.erase(0,6+n);
 result=std::move(frame);return 1;
}
// FLAP channel 1 cookie authentication for a redirected service connection.
inline bool makeCookieLogin(const std::string &cookie, uint16_t sequence, std::string &result) {
 if(cookie.empty() || cookie.size()>65531) return false;
 std::string p; wire32(p,1);wire16(p,6);wire16(p,uint16_t(cookie.size()));p+=cookie;
 return makeFlap({1,sequence,std::move(p)},result);
}
} // namespace bart
