/* OSCAR BART (SNAC family 0x0010) payload codec.
 * Standalone and deliberately free of Miranda / Windows dependencies.
 * The network layer must negotiate the BART service connection separately.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <limits>

namespace bart {
using Bytes = std::string;
struct AssetId { uint16_t type = 1; uint8_t flags = 0; Bytes hash; };
struct ImageReply { std::string screenName; AssetId id; uint8_t result = 0; Bytes image; };

inline void put16(Bytes &b, uint16_t n) { b.push_back(char(n >> 8)); b.push_back(char(n)); }
inline bool get8(const Bytes &b, size_t &p, uint8_t &n) {
 if (p >= b.size()) return false;
 n = uint8_t(b[p++]); return true;
}
inline bool get16(const Bytes &b, size_t &p, uint16_t &n) {
 if (b.size()-p < 2) return false;
 n = (uint16_t(uint8_t(b[p])) << 8) | uint8_t(b[p+1]); p += 2; return true;
}
inline bool parseId(const Bytes &b, size_t &p, AssetId &id) {
 uint16_t type; uint8_t flags, length;
 if (!get16(b,p,type) || !get8(b,p,flags) || !get8(b,p,length) || b.size()-p < length) return false;
 id.type=type; id.flags=flags; id.hash=b.substr(p,length); p += length; return true;
}
inline bool appendId(Bytes &b, const AssetId &id) {
 if (id.hash.empty() || id.hash.size()>255) return false;
 put16(b,id.type); b.push_back(char(id.flags)); b.push_back(char(id.hash.size())); b+=id.hash; return true;
}
// BART__DOWNLOAD2_QUERY (10/06): 1-byte SN length + SN + count + BART IDs.
inline bool makeDownload(const std::string &screenName, const AssetId &id, Bytes &payload) {
 if (screenName.empty() || screenName.size()>255) return false;
 Bytes b; b.push_back(char(screenName.size())); b+=screenName; b.push_back('\x01');
 if (!appendId(b,id)) return false;
 payload.swap(b); return true;
}
// BART__UPLOAD_QUERY (10/02): type + 16-bit length + image bytes.
inline bool makeUpload(uint16_t type, const Bytes &image, Bytes &payload) {
 if (image.empty() || image.size()>65535) return false;
 Bytes b; put16(b,type); put16(b,uint16_t(image.size())); b+=image;
 payload.swap(b); return true;
}
// BART__DOWNLOAD2_REPLY (10/07): SN, requested id, result code, returned id, data length, data.
inline bool parseDownloadReply(const Bytes &b, ImageReply &out) {
 size_t p=0; uint8_t n; uint16_t imageLength;
 ImageReply r;
 if (!get8(b,p,n) || n==0 || b.size()-p<n) return false;
 r.screenName=b.substr(p,n); p+=n;
 if (!parseId(b,p,r.id) || !get8(b,p,r.result)) return false;
 if (r.result!=0) { if(p!=b.size()) return false; out=std::move(r); return true; }
 AssetId returned;
 if (!parseId(b,p,returned) || returned.type!=r.id.type || returned.hash!=r.id.hash || !get16(b,p,imageLength)) return false;
 if (imageLength==0 || b.size()-p!=imageLength) return false;
 r.image=b.substr(p,imageLength);
 out=std::move(r); return true;
}
// Keep malformed and arbitrarily large images out of the avatar cache.
inline bool isSupportedImage(const Bytes &b) {
 if (b.size()<6 || b.size()>65535) return false;
 const unsigned char *s=reinterpret_cast<const unsigned char*>(b.data());
 return (s[0]==0xFF && s[1]==0xD8 && s[b.size()-2]==0xFF && s[b.size()-1]==0xD9)
     || (b.size()>=8 && b.compare(0,6,"GIF87a")==0)
     || (b.size()>=8 && b.compare(0,6,"GIF89a")==0)
     || (s[0]=='B' && s[1]=='M' && b.size()>=14)
     || (b.size()>=8 && s[0]==0x89 && b.compare(1,3,"PNG")==0 && s[4]==0x0D && s[5]==0x0A);
}
} // namespace bart
