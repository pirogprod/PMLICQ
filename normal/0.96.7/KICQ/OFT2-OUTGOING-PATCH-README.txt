OFT2 outgoing single-file integration draft (2026-10-30)

Changed: src/proto.cpp, src/proto.h. Existing OSCAR fileRendezvous packet is reused.

Implemented: SendFile() opens a listener, registers a transfer handle, issues channel-2 OSCAR Rendezvous, starts a worker, waits for TCP, sends OFT2 PROMPT / data / waits for DONE, and broadcasts Miranda ACKTYPE_FILE progress/result. FileCancel sets cancellation and shutdown joins workers.

LIMITATIONS: This is not yet a production ready or interoperability-tested transfer implementation. It supports one file only, direct TCP only; no publicly reachable address advertisement, NAT/relay/proxy negotiation, inbound file requests, multi-file or retry. The implementation has not been compiled with Visual Studio/Miranda SDK or tested against QIP, and might require API fixes. This build must not be advertised as complete interoperable file transfer. Other protocol message/status logic is unchanged.
