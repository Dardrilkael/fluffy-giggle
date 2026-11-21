#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ws/net/address.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/raw_tcp.hpp>

namespace {
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::string_view kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ws::net HTTP — web transport test</title>
  <style>
    :root { color-scheme: dark; font: 16px/1.5 system-ui, sans-serif; }
    body { margin: 0; min-height: 100vh; display: grid; place-items: center;
           background: #111827; color: #e5e7eb; }
    main { max-width: 42rem; margin: 2rem; padding: 2rem;
           border: 1px solid #374151; border-radius: 1rem; background: #1f2937; }
    h1 { margin-top: 0; color: #93c5fd; }
    code { color: #a7f3d0; }
  </style>
</head>
<body><main>
  <h1>ws::net HTTP web transport test</h1>
  <p>This page was served by the experimental <code>ws::net::RawTcpListener</code>.</p>
  <p>The browser is speaking ordinary HTTP/1.1 over an unframed TCP stream.</p>
</main></body>
</html>
)HTML";

std::string response(int status, std::string_view reason,
                     std::string_view body, bool head_only = false) {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " +
                    std::string(reason) + "\r\n";
  out += "Content-Type: text/html; charset=utf-8\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n";
  out += "X-Content-Type-Options: nosniff\r\n";
  out += "\r\n";
  if (!head_only) out.append(body);
  return out;
}

void serve(ws::net::RawTcpConnection& conn, std::string_view request) {
  const auto line_end = request.find("\r\n");
  if (line_end == std::string_view::npos) {
    const auto r = response(400, "Bad Request", "Malformed request\n");
    (void)conn.send(std::as_bytes(std::span{r.data(), r.size()}));
    (void)conn.close();
    return;
  }
  const std::string_view line = request.substr(0, line_end);
  const auto first = line.find(' ');
  const auto second = first == std::string_view::npos
                          ? first : line.find(' ', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos) {
    const auto r = response(400, "Bad Request", "Malformed request line\n");
    (void)conn.send(std::as_bytes(std::span{r.data(), r.size()}));
    (void)conn.close();
    return;
  }
  const auto method = line.substr(0, first);
  const auto target = line.substr(first + 1, second - first - 1);
  const bool head = method == "HEAD";
  std::string r;
  if ((method == "GET" || head) && (target == "/" || target == "/index.html")) {
    r = response(200, "OK", kPage, head);
  } else if (method != "GET" && !head) {
    r = response(405, "Method Not Allowed", "Method not allowed\n");
  } else {
    r = response(404, "Not Found", "Not found\n", head);
  }
  (void)conn.send(std::as_bytes(std::span{r.data(), r.size()}));
  (void)conn.close();
}
}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  if (argc > 1) host = argv[1];
  if (argc > 2) {
    try {
      const unsigned long parsed = std::stoul(argv[2]);
      if (parsed == 0 || parsed > 65535) throw std::out_of_range("port");
      port = static_cast<std::uint16_t>(parsed);
    } catch (...) {
      std::cerr << "Usage: http_server_web [host] [port]\n";
      return 2;
    }
  }

  ws::net::EventLoop loop;
  auto result = ws::net::RawTcpListener::listen(loop, {host, port});
  if (!result) {
    std::cerr << "Unable to listen: " << result.error().message << '\n';
    return 1;
  }
  auto listener = *result;
  std::vector<ws::net::RawTcpConnection::Ptr> clients;
  listener->set_on_connection([&](ws::net::RawTcpConnection::Ptr client) {
    auto* raw = client.get();
    auto pending = std::make_shared<std::string>();
    auto responded = std::make_shared<bool>(false);
    client->set_on_bytes([raw, pending, responded](std::span<const std::byte> bytes) {
      if (*responded) return;
      if (pending->size() + bytes.size() > kMaxHeaderBytes) {
        *responded = true;
        const auto r = response(413, "Content Too Large", "Request headers too large\n");
        (void)raw->send(std::as_bytes(std::span{r.data(), r.size()}));
        (void)raw->close();
        return;
      }
      pending->append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      if (pending->find("\r\n\r\n") != std::string::npos) {
        *responded = true;
        serve(*raw, *pending);
      }
    });
    client->set_on_closed([](ws::net::NetError) {});
    clients.push_back(std::move(client));
  });

  const auto bound = listener->local();
  std::cout << "ws::net HTTP HTTP test server: http://" << bound.host << ':'
            << bound.port << "/\nPress Ctrl+C to stop.\n";
  const auto ran = loop.run();
  listener->close();
  if (!ran) {
    std::cerr << ran.error().message << '\n';
    return 1;
  }
  return 0;
}
