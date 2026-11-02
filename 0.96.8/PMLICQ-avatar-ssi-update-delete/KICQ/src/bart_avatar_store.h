#pragma once
// BART reply validation and per-avatar cache naming. No Miranda API dependency.
#include "bart_codec.h"
#include <fstream>
#include <string>

namespace bart {
inline const char *imageExtension(const Bytes &b) {
 if (!isSupportedImage(b)) return nullptr;
 const auto *s = reinterpret_cast<const unsigned char *>(b.data());
 if (s[0] == 0xff && s[1] == 0xd8) return ".jpg";
 if (b.compare(0,3,"GIF") == 0) return ".gif";
 if (s[0] == 'B' && s[1] == 'M') return ".bmp";
 if (s[0] == 0x89 && b.compare(1,3,"PNG") == 0) return ".png";
 return nullptr;
}
inline std::string hexHash(const Bytes &hash) {
 static const char digits[] = "0123456789abcdef";
 std::string value;
 if (hash.empty() || hash.size() > 64) return value;
 value.reserve(hash.size() * 2);
 for (unsigned char b : hash) { value.push_back(digits[b >> 4]); value.push_back(digits[b & 15]); }
 return value;
}
inline bool cacheImage(const ImageReply &reply, const std::string &cacheDirectory,
                       std::string &savedPath) {
 if (reply.result != 0 || reply.id.hash.empty() || reply.id.type != 1 ||
     cacheDirectory.empty() || !imageExtension(reply.image)) return false;
 const auto name = hexHash(reply.id.hash);
 if (name.empty()) return false;
 // The caller creates a private cache directory; path is built only from a hex hash.
 std::string path = cacheDirectory;
 if (path.back() != '/' && path.back() != '\\') path += '/';
 path += name + imageExtension(reply.image);
 std::ofstream file(path, std::ios::binary | std::ios::trunc);
 if (!file) return false;
 file.write(reply.image.data(), static_cast<std::streamsize>(reply.image.size()));
 file.flush();
 if (!file.good()) return false;
 savedPath = path;
 return true;
}
}
