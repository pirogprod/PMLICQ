#pragma once
// OFT2 TCP transport primitives. The caller owns the socket and must negotiate
// OSCAR rendezvous and notify Miranda before using these operations.
#include "oft2_codec.h"
#include <winsock2.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace oft2 {
static inline bool send_all(SOCKET s, const void *data, size_t count) {
 const char *p = (const char *)data;
 while (count) {
  int n = send(s, p, (int)(count > 32768 ? 32768 : count), 0);
  if (n <= 0) return false;
  p += n; count -= n;
 }
 return true;
}
static inline bool recv_all(SOCKET s, void *data, size_t count) {
 char *p = (char *)data;
 while (count) {
  int n = recv(s, p, (int)(count > 32768 ? 32768 : count), 0);
  if (n <= 0) return false;
  p += n; count -= n;
 }
 return true;
}
static inline bool recv_header(SOCKET s, uint8_t (&packet)[HEADER_SIZE], Header &header) {
 return recv_all(s, packet, HEADER_SIZE) && decode(packet, HEADER_SIZE, header);
}
// OFT2 handshake helpers: 0x0101 PROMPT, 0x0202 ACK, 0x0204 DONE.
// Preserve all 256 bytes of the sender's PROMPT (including cookie, checksums,
// and filename); changing only the message type avoids corrupting metadata.
static inline bool send_reply(SOCKET s, const uint8_t (&prompt)[HEADER_SIZE], uint16_t type) {
 if (memcmp(prompt, "OFT2", 4) || be16(prompt + 4) != HEADER_SIZE)
  return false;
 if (type != 0x0202 && type != 0x0204) return false;
 uint8_t reply[HEADER_SIZE];
 memcpy(reply, prompt, HEADER_SIZE);
 put16(reply + 6, type);
 return send_all(s, reply, HEADER_SIZE);
}
static inline bool recv_expected(SOCKET s, uint16_t expected, uint8_t (&packet)[HEADER_SIZE], Header &header) {
 return recv_header(s, packet, header) && header.type == expected;
}
static inline bool send_prompt(SOCKET s, const uint8_t (&packet)[HEADER_SIZE]) {
 Header header;
 return decode(packet, HEADER_SIZE, header) && header.type == 0x0101 && send_all(s, packet, HEADER_SIZE);
}
// Transfer exactly 'size' bytes; never treat a short read/write as success.
// progress may be nullptr. A callback returning false cancels the transfer.
typedef bool (*Progress)(uint64_t transferred, uint64_t total, void *context);
static inline bool send_file(SOCKET s, FILE *f, uint64_t size, Progress progress, void *context) {
 if (!f) return false;
 uint8_t buffer[32768]; uint64_t done = 0;
 while (done < size) {
  size_t wanted = (size_t)((size - done) < sizeof(buffer) ? (size - done) : sizeof(buffer));
  size_t got = fread(buffer, 1, wanted, f);
  if (got != wanted || !send_all(s, buffer, got)) return false;
  done += got;
  if (progress && !progress(done, size, context)) return false;
 }
 return true;
}
static inline bool receive_file(SOCKET s, FILE *f, uint64_t size, Progress progress, void *context) {
 if (!f) return false;
 uint8_t buffer[32768]; uint64_t done = 0;
 while (done < size) {
  size_t wanted = (size_t)((size - done) < sizeof(buffer) ? (size - done) : sizeof(buffer));
  int got = recv(s, (char *)buffer, (int)wanted, 0);
  if (got <= 0 || fwrite(buffer, 1, (size_t)got, f) != (size_t)got) return false;
  done += (size_t)got;
  if (progress && !progress(done, size, context)) return false;
 }
 return fflush(f) == 0;
}
}
