#pragma once
// Windows TCP endpoint for OSCAR/OFT2 direct connections.
// Owns only the listening socket; accepted sockets are owned by the caller.
// The endpoint must be created BEFORE a channel-2 offer is sent.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>

namespace oft2 {
class TcpListener {
 SOCKET listener_ = INVALID_SOCKET;
 uint16_t port_ = 0;
public:
 TcpListener() = default;
 TcpListener(const TcpListener&) = delete;
 TcpListener& operator=(const TcpListener&) = delete;
 ~TcpListener() { close(); }
 uint16_t port() const { return port_; }
 bool open(uint32_t address = INADDR_ANY) {
  close();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(address);
  addr.sin_port = 0; // let OS allocate an unused port
  if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
      listen(s, 1) == SOCKET_ERROR) {
   closesocket(s); return false;
  }
  int len = sizeof(addr);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
   closesocket(s); return false;
  }
  listener_ = s;
  port_ = ntohs(addr.sin_port);
  return true;
 }
 // Timeout is in milliseconds. Returns INVALID_SOCKET on timeout, cancel or error.
 // Avoid blocking the Miranda network/UI threads: call only from a worker.
 SOCKET accept_one(uint32_t timeoutMs, const volatile LONG *cancel = nullptr) {
  if (listener_ == INVALID_SOCKET) return INVALID_SOCKET;
  DWORD remaining = timeoutMs;
  while (remaining) {
   if (cancel && InterlockedCompareExchange(const_cast<volatile LONG*>(cancel), 0, 0)) return INVALID_SOCKET;
   DWORD slice = remaining < 200 ? remaining : 200;
   fd_set readfds;
   FD_ZERO(&readfds); FD_SET(listener_, &readfds);
   timeval tv = { long(slice / 1000), long((slice % 1000) * 1000) };
   int ready = select(0, &readfds, nullptr, nullptr, &tv);
   if (ready == SOCKET_ERROR) return INVALID_SOCKET;
   if (ready > 0) {
    sockaddr_in peer = {};
    int len = sizeof(peer);
    return accept(listener_, reinterpret_cast<sockaddr*>(&peer), &len);
   }
   remaining -= slice;
  }
  return INVALID_SOCKET;
 }
 void close() {
  if (listener_ != INVALID_SOCKET) closesocket(listener_);
  listener_ = INVALID_SOCKET;
  port_ = 0;
 }
};
}
