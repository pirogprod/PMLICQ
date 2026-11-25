#pragma once
// Blocking OFT2 data phase, intended to run only on a dedicated worker thread.
// This is not a standalone OSCAR transfer: the caller must first negotiate a
// channel-2 rendezvous and own the connected TCP socket.
#include "oft2_transport.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace oft2 {
// Sender-only stage trace; 0=validation, 1=PROMPT, 2=ACK,
// 3=data, 4=DONE, 5=finished. The receiver code remains untouched.
enum TransferStage { ST_VALIDATE=0, ST_PROMPT=1, ST_ACK=2, ST_DATA=3, ST_DONE=4, ST_FINISHED=5 };
// The caller supplies the OFT2 PROMPT generated from the actual file metadata.
// Sending raw data before ACK is forbidden. A successful return requires DONE.
static inline bool send_one(SOCKET socket, const uint8_t (&prompt)[HEADER_SIZE],
                            FILE *input, uint64_t length, Progress progress = nullptr,
                            void *ctx = nullptr, int *stage = nullptr, uint64_t *dataSent = nullptr) {
 if (stage) *stage = ST_VALIDATE;
 if (dataSent) *dataSent = 0;
 Header h, ack;
 uint8_t response[HEADER_SIZE];
 if (!decode(prompt, HEADER_SIZE, h) || h.type != 0x0101 ||
     h.size != length || h.totalFiles != 1 || h.filesLeft != 1 ||
     length > UINT32_MAX || !input) return false;
 if (stage) *stage = ST_PROMPT;
 if (!send_prompt(socket, prompt)) return false;
 if (stage) *stage = ST_ACK;
 if (!recv_expected(socket, 0x0202, response, ack)) return false;
 // Never trust a peer acknowledgement for a different file.
 if (memcmp(response + 8, prompt + 8, 8) != 0 ||
     ack.size != h.size || ack.totalFiles != h.totalFiles ||
     ack.filesLeft != h.filesLeft || ack.received != 0) return false;
 if (stage) *stage = ST_DATA;
 if (!send_file(socket, input, length, progress, ctx, dataSent)) return false;
 Header done;
 if (stage) *stage = ST_DONE;
 if (!recv_expected(socket, 0x0204, response, done)) return false;
 const bool validDone = memcmp(response + 8, prompt + 8, 8) == 0 &&
        done.totalFiles == h.totalFiles && done.filesLeft == h.filesLeft &&
        done.size == h.size && done.received == length;
 if (validDone && stage) *stage = ST_FINISHED;
 return validDone;
}

// The receiver writes only the exact announced number of bytes. A temporary
// path should be opened by the caller; publish/rename only after DONE succeeds.
static inline bool receive_one(SOCKET socket, FILE *output, uint64_t maxSize,
                               Progress progress = nullptr, void *ctx = nullptr) {
 Header h;
 uint8_t prompt[HEADER_SIZE];
 if (!output || !recv_expected(socket, 0x0101, prompt, h) ||
     h.filesLeft != 1 || h.totalFiles != 1 || h.size > maxSize) return false;
 if (!send_reply(socket, prompt, 0x0202)) return false;
 if (!receive_file(socket, output, h.size, progress, ctx)) return false;
 uint8_t done[HEADER_SIZE];
 memcpy(done, prompt, HEADER_SIZE);
 put16(done + 6, 0x0204);
 put32(done + 60, h.size);
 return send_all(socket, done, HEADER_SIZE);
}
}
