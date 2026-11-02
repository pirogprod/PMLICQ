# KICQ: Open OSCAR Server / iserverd compatibility patch

This is a source-level patch to the KICQ protocol. It has **not** been compiled or integration-tested against either server.

KICQ already implements legacy OSCAR login (FLAP channel 1 XOR login), BOS redirect, messaging, SSI and presence. Server compatibility depends on the deployed server's support for these dialects.

## Account settings

Existing Miranda KICQ account options expose the login Server and Port. Configure these to point to your server (typically port 5190).

Additional per-account database keys (module: your KICQ account module name):

- `BosMode` (DWORD): `0` follow advertised BOS address (default); `1` use login server host with advertised BOS port; `2` use `BosServer` with advertised BOS port.
- `BosServer` (Unicode string): reachable BOS hostname/IP for `BosMode=2`.
- `BosPort` (DWORD): optional override for the advertised BOS port; `0` keeps advertised port.

These advanced keys are not yet exposed in the settings UI; use a Miranda database editor and restart the account connection after editing.

## Building

The repository targets Windows / MSVC and Miranda NG SDK. Build the KICQ project in the supplied Visual Studio solutions for Win32 and x64 with matching dependencies. The Linux environment used for this patch does not contain the required Windows MSVC toolchain, so no DLL is included.

## Limitations

No claim of working authentication with iserverd or Open OSCAR Server is made without end-to-end tests. In particular, differences in server authentication dialect and SSI behavior may require further work.
