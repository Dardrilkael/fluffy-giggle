#pragma once

// Internal: the libuv UDP backend - the datagram half of the core seam
// (core/transport.hpp). Same ctx-outlives-object discipline as the
// stream half: the heap DatagramCtx owns the uv_udp_t, frees itself
// only in the uv close callback, and a keepalive shared_ptr bridges
// the public object across that window.

#include <cstddef>
#include <memory>
#include <span>

#include <uv.h>

#include "core/transport.hpp"
#include <ws/core/result.hpp>
#include <ws/net/address.hpp>

namespace ws::net::detail {

class UvLoopBackend;

class UvDatagramSocket final : public IDatagram {
 public:
  ~UvDatagramSocket() override;  // detaches; never frees a live handle

  void attach_sink(IDatagramSink* sink) override;
  void set_keepalive(std::shared_ptr<void> owner) override;
  [[nodiscard]] ws::core::Status send_to(const Address& to,
                                         std::span<const std::byte> data)
      override;
  void close() override;
  [[nodiscard]] Address local() const override;

  // Internal: false when construction failed (the factory turns an
  // inert socket into a nullptr result).
  [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }

  // Internal: the ctx close callback calls this right before freeing
  // itself - the object must never touch the ctx again.
  void detach() noexcept { ctx_ = nullptr; }

 private:
  friend std::unique_ptr<IDatagram> make_datagram(ILoopBackend& loop,
                                                  const Address& local,
                                                  bool reuse);

  UvDatagramSocket(UvLoopBackend& backend, const Address& local, bool reuse);

  struct DatagramCtx;  // defined in uv_udp.cpp
  struct SendReq;      // ditto

  static void on_alloc(uv_handle_t* handle, std::size_t /*suggested*/,
                       uv_buf_t* buf);
  static void on_recv(uv_udp_t* udp, ssize_t nread, const uv_buf_t* buf,
                      const sockaddr* addr, unsigned /*flags*/);
  static void on_sent(uv_udp_send_t* req, int /*status*/);
  static void on_udp_closed(uv_handle_t* handle);

  DatagramCtx* ctx_ = nullptr;
};

}  // namespace ws::net::detail
