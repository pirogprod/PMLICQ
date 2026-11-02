PMLICQ Open OSCAR Server presence patch — Miranda NG 0.96.8

SOURCE ONLY — no compiled DLL. Not verified against a live server.

Changes compared with the last status-retry source archive:
- During BOS login, activate SSI roster before publishing status.
- Remove automatic away-text SNAC 01/1E during login.
- Use minimal TLV 0x0006 (4 bytes) for initial presence and later status changes.
- Preserve earlier keepalive, full FLAP send, and buddy subscription changes.

IMPORTANT: Open OSCAR Server previously reported EOF on SNAC 01/1E.
This patch isolates optional TLVs; it is an experimental fix, not a confirmed cure.

For Miranda NG 0.96.7: this archive DOES NOT provide a 0.96.7-compatible
SDK or binary. Build against Miranda NG 0.96.7 headers/libs; a 0.96.8
compile will not necessarily load in 0.96.7.

Use: back up protocols/KICQ; replace its contents with KICQ/ from this
archive; compile Release x64. Output remains PMLICQ.dll, internal KICQ.

Test: start offline then online; start away; online->away->NA->DND->online;
check server logs and presence subscriptions. Keep a working backup.
