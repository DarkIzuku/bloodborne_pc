# Online with a custom shadNet server

Start `BloodborneLauncher.exe`, open ONLINE, enable online play and enter the
server host, TCP port and complete WebAPI URL, including its port. No server is
selected by default. The game API uses that same URL unless you explicitly
provide a separate API. Enter the server's account website URL to open its
registration page, then use your account name and password in the launcher.
Save and press PLAY. The game still has its own Play Online menu.

The launcher checks the shadNet protocol greeting and Bloodborne's `ss.info`
bootstrap. A successful check establishes reachability and routing; it does not
establish a successful account login or multiplayer session. The bootstrap's API
origins must match the selected game API. Configure the server's `PublicBaseUrl`
to advertise its reachable URL and port.

The launcher prepares `network/host_overrides.json` automatically and directs
the runtime to this exact file. There is no dependency on shadPS4's profile or
Roaming directory. It redirects Bloodborne's Sony status origin to the selected
game API, retaining the game's request paths. This version therefore requires
the game API to be served at its host root. Use an IPv4 address or DNS name;
IPv6 P2P support is not implemented by this native bridge.

UPnP is optional. The local UDP P2P port can be chosen or left blank for
automatic allocation. Matching2 and signaling capabilities are negotiated with
the server. Server build versions are not pinned: servers supporting shadNet
protocol v1 and the required APIs can be updated without rebuilding this
client. A future incompatible wire protocol still requires a client update.

Remembering the account is optional and uses Windows DPAPI. Passwords and tokens
never enter `bbport.ini` or `launcher-settings.json`. Copying a portable build to
another Windows account requires reentering the credentials. Offline mode clears
inherited online settings before starting the runtime.

Two-client summoning, invasions and NAT traversal require separate gameplay
validation. Invitations outside the game are not exposed by this launcher.
