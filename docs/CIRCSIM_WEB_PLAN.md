# CIRCSIM web server experiment

Branch: `html-test`

## Finding: the current `ws::net` TCP API is not HTTP-compatible

`ws::net::TcpListener` and `TcpConnection` expose a message-oriented protocol.
Every `TcpConnection::send()` adds a 4-byte big-endian length prefix, and
`ConnectionLogic` parses that prefix before delivering `on_message`.

An ordinary browser sends HTTP/1.1 bytes directly on the TCP stream
(`GET / HTTP/1.1\\r\\n...`) and expects an unframed HTTP response. It does
not know about this workspace-specific prefix. Therefore an HTTP server
cannot correctly be built on top of the current public `TcpListener` API;
trying to do so would only work with a custom framed client, not Chrome,
Firefox, or curl.

The lower-level transport already has the right capability:
`detail::IStreamPeer` delivers arbitrary byte chunks through
`IStreamSink::on_bytes()` and writes arbitrary byte vectors. The framing
is added above that seam by `ConnectionLogic`, not by libuv. This is a
good architectural seam for adding a raw byte-stream API without exposing
libuv or creating a second socket backend.

## Proposed implementation

1. Add public raw stream types (for example `RawTcpListener` and
   `RawTcpConnection`) alongside the existing framed types. Keep their
   callbacks on the same `EventLoop` thread and preserve the current
   keepalive/close-callback lifetime guarantees.
2. Reuse `detail::IStreamPeer` / `detail::IStreamAcceptor` directly.
   The raw connection forwards `on_bytes` to the application and writes
   bytes without calling `append_frame` or `FrameParser`.
3. Add `apps/circsim_web`, a small HTTP/1.1 server using only the raw
   `ws::net` API. Start with GET/HEAD, static HTML/CSS/JS assets, MIME
   types, Content-Length, connection-close responses, request-size limits,
   and path traversal protection.
4. Keep the HTTP parser and response builder separate from socket/event-loop
   code so they can be tested as pure functions. Add loopback integration
   tests using a real HTTP client (curl or a socket test), not the framed
   `TcpConnection` client.
5. Serve CIRCSIM's existing browser-facing UI only after the web frontend
   exists. The current `apps/circsim_editor` is a native `ws::ui`
   application; it is not an HTML bundle that can simply be hosted.

## First vertical slice

- Bind to `127.0.0.1:8080` by default, with host/port CLI overrides.
- Serve a minimal `index.html` and static assets.
- Return 400 for malformed requests, 404 for missing files, 405 for
  unsupported methods, and 413 for oversized headers/body.
- Do not add WebSocket, TLS, dynamic schematic APIs, or a browser-side
  circuit editor in this first slice.

## Current status

This commit records the transport compatibility finding and implementation
boundary. It deliberately does not pretend the framed API is HTTP or add a
libuv dependency to an app. The next code change should add the raw stream
public API to `libs/net`, then build the HTTP server on that API.
