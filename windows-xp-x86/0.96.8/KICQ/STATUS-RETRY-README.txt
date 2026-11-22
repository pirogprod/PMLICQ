PMLICQ OSCAR status retry source for Miranda NG 0.96.8

Change: during a live status transition send the full setDcInfo SNAC 01/1E payload, the same layout sent during initial BOS login, instead of the minimal status-only TLV.
This is an untested compatibility hypothesis for Open OSCAR Server unexpected EOF on OServiceSetUserInfoFields. The minimal TLV is structurally valid; root cause remains unconfirmed.
Existing FLAP partial-send, keepalive and presence subscription changes are retained.

BUILD: use the Miranda NG 0.96.8 matching SDK/source and MSVC on Windows; Release x64 target PMLICQ.dll.
WARNING: this archive alone does not provide 0.96.7 binary compatibility; project is still based on 0.96.8-era source.
No compiled DLL is included. Keep KICQ internal protocol name unchanged.
