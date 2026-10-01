# libs/net - CLAUDE.md

Networking in the workspace: a single-threaded event loop, TCP client/server
with length-prefixed message framing, UDP, and LAN discovery - all behind a
Result-only API. Consumers see ws::net names only; libuv never appears
under include/ (it lives behind pimpls, included only by src/backend/uv).

## Purpose
- `visibility.hpp`: `WS_NET_API` - export control (dllexport/dllimport on
  MSVC, default visibility everywhere else, hidden as the compiler default).
- `errors.hpp`: `NetError` codes + `describe(NetError)` (the static message
  table) + `fail(NetError, msg)` (the `unexpected` form, Result paths only).
- `address.hpp`: `Address` - ws-owned host/port (parse/loopback/any/
  broadcast); no sockaddr ever crosses this boundary.
- `event_loop.hpp`: `EventLoop` - run/stop, `post()` (the ONE thread-safe
  call), `TimerHandle` timers.
- `tcp.hpp`: `TcpListener`/`TcpConnection` - message API over framed TCP.
- `udp.hpp`: `UdpSocket` - bind/send_to/on_packet.
- `discovery.hpp`: `DiscoveryAnnouncer`/`DiscoveryBrowser` - LAN discovery
  over UDP broadcast.

## Allowed dependencies
- `ws::core` (Result/Status, logging) - PUBLIC.
- libuv v1.53.0 (async I/O) - PRIVATE: only `src/backend/uv` includes
  `uv.h`; `cmake/ThirdParty.cmake` fetches it STATIC for every platform.
  The backend is swappable BY DESIGN: core logic talks only to the
  `ILoopBackend`/`IStreamPeer`/`IDatagram` seam (`src/core/transport.hpp`),
  so a future WSAPoll/epoll backend (`src/backend/poll`, say) changes no
  public header and no user code.

## Invariants / rules
- Error handling: public APIs return `ws::core::Result`/`Status` (root
  CLAUDE.md); codes are `NetError` values, category always "net", no
  exceptions cross the boundary. `ws::net::fail()` is the enum-taking
  local - core's raw-code `fail` is hidden inside ws::net.
- Callbacks NEVER allocate to report errors: they carry `NetError`, and
  every `string_view` they can see comes from `describe()`'s static table -
  valid forever, never a per-call buffer. Only Result-returning APIs build
  `std::string` messages.
- Threading: exactly one thread runs the loop; every callback fires on it.
  `post()` is the one thread-safe call (wake + FIFO onto the loop thread).
- Lifetime: the loop keeps every connection/listener/socket alive until
  its close callback has actually run - dropping your handle mid-flight
  never frees a live backend handle. Every uv handle is closed via
  `uv_close` and freed only in its close callback (RAII discipline lives in
  `src/backend/uv`); `EventLoop`'s destructor drains before freeing.
- Message spans: `on_message` delivers a `std::span<const std::byte>` into
  the connection's read buffer, valid ONLY during the callback. Debug
  builds poison the delivered bytes (0xCD) right after the callback
  returns, so a stashed span reads garbage on its next access - see the
  deliberate "do not do this" case in `tests/test_frame.cpp`.
- Public headers include only the standard library and `ws/` headers.

## Known tradeoffs
- `TcpConnection::send()` COPIES the payload into the write queue before
  returning (the caller's buffer is free to die with the call). A
  zero-copy send is a possible future addition if profiling ever shows the
  copy matters; not implemented now.
- UDP is best-effort END TO END and the API owns that: `UdpSocket::
  send_to` fails synchronously only (closed socket, bad address,
  over-cap payload); asynchronous outcomes - an ICMP "port unreachable"
  answering an earlier send, a datagram lost on the wire - are
  deliberately unreported (the backend swallows the recv-side
  UV_ECONNRESET Windows raises for it, and the send callback ignores
  its status). Discovery inherits the same silence: a beat that never
  lands simply means "not seen lately". Confirmed delivery is TCP's job.
- TLS/WAN/reliable-UDP are out of scope v1. The seam above is where they
  would land (TLS wraps `IStreamPeer`), so none of them will change the
  public headers either.

## Tests
ctest --preset debug -R net
Loopback tests need nothing beyond the machine itself (127.0.0.1,
ephemeral ports); the socket-free core tests run everywhere.
