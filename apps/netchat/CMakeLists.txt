# Terminal chat over ws::net: every running copy is both listener and
# dialer. Each announces itself by discovery beat - LAN broadcast for
# peers on other machines, a loopback-directed broadcast (127.255.255.255,
# which never leaves this machine) for peers in other terminals here -
# and the copy with the smaller listen port dials, so every pair of
# terminals ends up with exactly one framed TCP connection. First frame
# on any connection is HELLO, everything after is chat. Console app,
# gated desktop-only in apps/CMakeLists.txt.
ws_add_app(netchat DEPENDS ws::net)
