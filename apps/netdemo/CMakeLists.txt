# Headless ws::net example: one EventLoop, every feature of the library
# exercised once - framed TCP over loopback (listener + client, echoing
# server side), a UDP round trip, LAN discovery broadcasting on the
# default port - then an explicit in-order teardown that closes live
# handles with loop work still in flight. Console app like wavplayer;
# gated desktop-only in apps/CMakeLists.txt.
ws_add_app(netdemo DEPENDS ws::net)
