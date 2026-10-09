# Native shadNet bridge

The network-only source in `gpu/bbnet/` is adapted from
https://github.com/Supermedo/bloodborne_pc/tree/e0761196535bac06e5d5beb1c43da5124a06dd4c
(the `windows` branch inspected on 2026-10-09). Original RPCS3, shadPS4 and
shadp2p notices remain in the files. These portions are GPL-2.0-or-later.
`third_party/httplib/httplib.h` and `third_party/wepoll/` retain their embedded
third-party license notices.

The renderer, shaders and upscalers were not imported. The bridge reuses this
project's symbol registry, guest-thread/TLS services, game metadata and guarded
Windows HLE entries. Its settings and compatibility headers are private to the
network target; the renderer's settings and thread class remain authoritative.
Registration and session initialization are separate: the session starts after
validated game metadata, guest TLS and linked modules are available.

No server address or account is provided by default. The WPF launcher and
runtime read the same `bbport.ini` online profile. Runtime redirects are generated
in the application's local `network/` cache. Passwords and optional validation
tokens are excluded from INI/JSON settings and can be stored through Windows
DPAPI for the current Windows user.
