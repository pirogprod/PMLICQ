# Experimental OSCAR interoperability fixes (source only)

Changes in `src/server.cpp`:

1. FLAP writes now loop on partial `Netlib_Send` results, preventing truncated packets when the socket reports a short write.
2. Keepalive uses the OSCAR transport FLAP channel 5 with an empty payload, rather than sending an unrelated ICBM SNAC 04/06 and disconnecting when no SNAC reply arrives.
3. After SSI roster loading, the client explicitly subscribes to contact presence with BuddyAddBuddies SNAC 03/04, batching UINs in small packets.

**Not compiled or tested** in this environment. Back up your existing DLL and profile before testing. Inspect Miranda's Netlib behavior on your SDK version. These fixes do not guarantee resolution of every `unexpected EOF` error.

The login identification remains unchanged: `xorLogin()` already advertises an ICQ 2000b client string, while `setUserInfo()` sends a Jasmine compatibility capability. Changing both without a verified server expectation risks introducing new incompatibilities. No protocol names, GUIDs, or target DLL names were changed.

Build `Release x64` against Miranda NG 0.96.8. In the project file the target remains `PMLICQ`.
