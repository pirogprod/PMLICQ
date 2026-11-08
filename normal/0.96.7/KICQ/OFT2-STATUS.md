# OSCAR/OFT2 integration status

The new `src/oft2_socket_transfer.h` provides a checked, single-file OFT2
TCP data phase with PROMPT/ACK/DONE and progress cancellation. It intentionally
requires an already established rendezvous socket; it is **not** wired to
Miranda's File API or to `SendFile`, `FileAllow`, `FileDeny`, `FileCancel`.

Not yet implemented: channel-2 invitation/reply dispatch, local listener,
NAT/proxy handling, file-dialog notification, transfer callbacks, multi-file
negotiation, and end-to-end compatibility testing. Do not advertise PF1_FILESEND
or ship this source as a working file-transfer plugin.
