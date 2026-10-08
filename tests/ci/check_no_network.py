"""Fail if a Windows binary imports, delay-loads or names any networking library.

Usage: python check_no_network.py <file.dll|file.exe> [...]

The VST3-only build compiles JUCE's networking out (JUCE_ATK_NO_NETWORK), so neither the OBS
module nor the scanner should reference WinINet, WinSock, WinHTTP or similar libraries.
"""

import sys

import pefile

NETWORK_DLLS = {
    "wininet.dll",
    "winhttp.dll",
    "urlmon.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "mswsock.dll",
    "iphlpapi.dll",
    "dnsapi.dll",
    "netapi32.dll",
    "mapi32.dll",
    "websocket.dll",
    "httpapi.dll",
    "rasapi32.dll",
    "wlanapi.dll",
    "winnsi.dll",
}


def embedded_names(data):
    """Library names present as ASCII or UTF-16 strings (e.g. for LoadLibrary)."""
    lower = data.lower()
    found = set()
    for name in NETWORK_DLLS:
        stem = name[: -len(".dll")]
        if stem.encode("ascii") in lower or stem.encode("utf-16-le") in lower:
            found.add(name)
    return found


def check(path):
    pe = pefile.PE(path)
    imports = sorted({i.dll.decode().lower() for i in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])})
    delay = sorted({i.dll.decode().lower() for i in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])})
    data = open(path, "rb").read()

    print(f"{path}\n  imports: {', '.join(imports)}\n  delay-load: {', '.join(delay) or '-'}")

    problems = []
    problems += [f"imports {d}" for d in imports if d in NETWORK_DLLS]
    problems += [f"delay-loads {d}" for d in delay if d in NETWORK_DLLS]
    problems += [f"contains the library name {d}" for d in sorted(embedded_names(data))]

    for p in problems:
        print(f"  NETWORK: {p}")
    print("  OK: no networking libraries" if not problems else "  FAILED")
    return not problems


if __name__ == "__main__":
    results = [check(p) for p in sys.argv[1:]]
    sys.exit(0 if results and all(results) else 1)
