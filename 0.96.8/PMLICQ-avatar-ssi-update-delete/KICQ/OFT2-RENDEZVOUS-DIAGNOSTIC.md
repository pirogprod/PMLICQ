# OFT2 signalling correction

The outgoing file proposal previously advertised only a TCP port. It now adds
request-number TLV 0x000A and client IPv4 TLV 0x0002 plus the local IP TLV 0x0003.
TLV 0x0004 is server-verified IP and must not be set by the client.

The interface IP is selected by a UDP route lookup to the default OSCAR server.
This does not discover a public address through NAT, or guarantee that the port
is reachable. Port forwarding, a relay/proxy, and explicit recipient acceptance
may still be required. Incoming offers are still log-only in server.cpp, so
incoming file requests do not pop up in Miranda NG in this build.

Standalone portable rendezvous codec test: g++ -std=c++17 test_rendezvous.cpp
The Windows plugin was NOT built or live-tested in this environment.
