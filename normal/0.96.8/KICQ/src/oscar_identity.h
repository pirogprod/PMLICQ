#pragma once
#include <string>
#include <cstdint>
// OSCAR names are case-insensitive and ignore ASCII spaces. Numeric UINs
// remain numeric for ICQ-only services; never parse a prefix of an AIM name.
namespace oscarid {
inline uint32_t uin(const std::string &s) {
 if (s.empty()) return 0;
 uint64_t n=0;
 for (char c:s) { if(c<'0'||c>'9') return 0; n=n*10+(c-'0'); if(n>0xffffffffULL) return 0; }
 return static_cast<uint32_t>(n);
}
inline std::string key(const std::string &s) {
 std::string k;
 for (unsigned char c:s) {
  if(c==' ') continue;
  if(c>='A'&&c<='Z') c+=32;
  if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='@'||c=='.'||c=='_'||c=='-')) return {};
  k+=char(c);
 }
 if(k.empty()||k.size()>97) return {};
 bool numeric=true; for(char c:k) if(c<'0'||c>'9') numeric=false;
 if(numeric) { auto n=uin(k); return n?std::to_string(n):std::string(); }
 return k;
}
}
