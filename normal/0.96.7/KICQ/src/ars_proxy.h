#pragma once
// PMLICQ ARS relay transport, independent of Miranda callbacks and data model.
// OSCAR ARS protocol 0x044A / allocate=2 / ready=5 / join=4.
// The caller owns the socket; never free a socket from a second thread.
#include "oft2_rendezvous.h"
#include "oft2_transport.h"
#include <string>
#include <vector>
#include <stdint.h>
#include <limits.h>

namespace ars {
static const int kTimeoutMs = 120000;
static const int kChunkLimit = 1024;
static inline void u16(std::string &b, unsigned v) { b.push_back(char(v >> 8)); b.push_back(char(v)); }
static inline void u32(std::string &b, uint32_t v) { for (int i=3;i>=0;--i) b.push_back(char(v >> (i*8))); }
static inline unsigned r16(const unsigned char *p) { return (unsigned(p[0]) << 8) | p[1]; }
static inline uint32_t r32(const unsigned char *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static inline bool message(std::string &result, unsigned command, const std::string &payload) {
 std::string body; u16(body, 0x044A); u16(body, command); u32(body, 0); u16(body, 0); body += payload;
 if (body.size() > 0xFFFF) return false;
 result.clear(); u16(result, (unsigned)body.size()); result += body; return true;
}
static inline bool encode(std::string &out, const std::string &uin, const std::array<unsigned char,8> &cookie,
                          const std::string &cap, bool join, unsigned port) {
 if (uin.empty() || uin.size() > 255 || cap.size() != 16 || (join && (port == 0 || port > 65535))) return false;
 std::string p; p.push_back(char(uin.size())); p += uin;
 if (join) u16(p, port);
 p.append((const char *)cookie.data(), cookie.size()); u16(p, 1); u16(p, 16); p += cap;
 return message(out, join ? 4 : 2, p);
}
static inline bool recvMessage(SOCKET s, unsigned &command, std::string &body) {
 unsigned char sz[2];
 if (!oft2::recv_all(s, sz, 2)) return false;
 unsigned n = r16(sz);
 if (n < 10 || n > kChunkLimit) return false; // reject oversized attacker-controlled frames
 body.resize(n);
 if (!oft2::recv_all(s, &body[0], n)) return false;
 const unsigned char *p = (const unsigned char *)body.data();
 if (r16(p) != 0x044A) return false;
 command = r16(p + 2);
 return true; // Error command 0x0001 must be reported to caller, not silently hidden.
}
// An ARS error has the same framed header as a successful response, but
// command 0x0001 carries a two-byte error code (e.g. 0x000d bad request).
// Keep these outputs optional so existing receiver-side call sites are intact.
static inline bool handshakeAllocate(SOCKET s, const std::string &uin, const std::array<unsigned char,8> &cookie,
                         const std::string &cap, uint32_t &ip, uint16_t &port,
                         unsigned *responseCommand = nullptr, unsigned *proxyError = nullptr) {
 std::string packet, body;
 unsigned cmd = 0;
 if (responseCommand) *responseCommand = 0;
 if (proxyError) *proxyError = 0;
 // The classic ARS server may take up to ten seconds to answer a bad login;
 // an 8s timeout keeps the test bounded while allowing slower relays.
 DWORD allocateTimeout = 8000;
 setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&allocateTimeout, sizeof(allocateTimeout));
 const bool received = encode(packet, uin, cookie, cap, false, 0) &&
   oft2::send_all(s, packet.data(), packet.size()) && recvMessage(s, cmd, body);
 DWORD dataTimeout = kTimeoutMs;
 setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dataTimeout, sizeof(dataTimeout));
 if (responseCommand) *responseCommand = cmd;
 if (!received) return false;
 if (cmd == 1 && body.size() >= 12) {
  if (proxyError) *proxyError = r16((const unsigned char *)body.data() + 10);
  return false;
 }
 if (cmd != 3 || body.size() != 16) return false;
 const unsigned char *p = (const unsigned char *)body.data();
 port = (uint16_t)r16(p + 10); ip = r32(p + 12);
 return port != 0 && ip != 0;
}
static inline bool handshakeJoin(SOCKET s, const std::string &uin, const std::array<unsigned char,8> &cookie,
                      const std::string &cap, uint16_t port) {
 std::string packet, body; unsigned cmd = 0;
 // A rejected JOIN must not occupy the entire 120-second sender timeout.
 // Restore the data-phase timeout after a successful handshake.
 DWORD handshakeTimeout = 5000;
 setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&handshakeTimeout, sizeof(handshakeTimeout));
 bool ok = encode(packet, uin, cookie, cap, true, port) &&
           oft2::send_all(s, packet.data(), packet.size()) &&
           recvMessage(s, cmd, body) && cmd == 5;
 DWORD dataTimeout = kTimeoutMs;
 setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dataTimeout, sizeof(dataTimeout));
 return ok;
}
static inline bool waitReady(SOCKET s, unsigned *lastResponse = nullptr) {
 unsigned command = 0; std::string body;
 const bool received = recvMessage(s, command, body);
 if (lastResponse) *lastResponse = command;
 return received && command == 5;
}
static inline bool connectTo(const char *host, uint16_t port, SOCKET &result) {
 result = INVALID_SOCKET;
 if (!host || !*host || port == 0) return false;
 char portstr[8]; sprintf_s(portstr, "%u", (unsigned)port);
 addrinfo hints = {}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
 addrinfo *list = nullptr;
 if (getaddrinfo(host, portstr, &hints, &list) != 0) return false;
 for (addrinfo *it = list; it; it = it->ai_next) {
  SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
  if (s == INVALID_SOCKET) continue;
  u_long nonblock=1;
  if (ioctlsocket(s, FIONBIO, &nonblock) != 0) { closesocket(s); continue; }
  int r=connect(s, it->ai_addr, (int)it->ai_addrlen);
  if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
   fd_set writes, errors; FD_ZERO(&writes); FD_ZERO(&errors); FD_SET(s, &writes); FD_SET(s, &errors);
   timeval tv = { 5, 0 }; // Do not stall initial OSCAR file invitation for 15 seconds
   r = select(0, nullptr, &writes, &errors, &tv);
   if (r > 0 && FD_ISSET(s, &writes)) { int err=0; int len=sizeof(err); r = (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len)==0 && !err) ? 0 : SOCKET_ERROR; }
   else r=SOCKET_ERROR;
  }
  nonblock=0; ioctlsocket(s, FIONBIO, &nonblock);
  if (r == 0) {
   DWORD timeout = kTimeoutMs;
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
   result=s; break;
  }
  closesocket(s);
 }
 freeaddrinfo(list); return result != INVALID_SOCKET;
}
static inline std::string ip4(uint32_t ip) {
 char out[20]; sprintf_s(out, "%u.%u.%u.%u", ip >> 24, (ip >> 16)&255, (ip >> 8)&255, ip&255);
 return out;
}
}
