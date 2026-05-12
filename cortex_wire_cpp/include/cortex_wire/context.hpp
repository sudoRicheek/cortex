#ifndef CORTEX_WIRE__CONTEXT_HPP_
#define CORTEX_WIRE__CONTEXT_HPP_

#include <zmq.hpp>

#include <memory>

namespace cortex_wire
{

// Shared ownership wrapper around `zmq::context_t`. Copying a Context is cheap
// (a refcount bump) and lets multiple Publisher / Subscriber instances share
// the same underlying ZMQ context.
//
// The context lives as long as any Context copy referencing it does. On the
// last destruction we run shutdown + close in the right order to unblock any
// recv()s and tear the context down cleanly.
class Context
{
public:
  // Construct a fresh context with one IO thread (matches the Python default).
  Context()
  : ctx_(std::shared_ptr<zmq::context_t>(
        new zmq::context_t(1),
        [](zmq::context_t * c) {
          c->shutdown();
          c->close();
          delete c;
        }))
  {
  }

  // Wrap an externally-owned context. The Context will NOT shutdown / close
  // the underlying context on destruction; that remains the caller's job.
  // Useful for interop with code that already owns a zmq::context_t.
  explicit Context(zmq::context_t & external) noexcept
  : ctx_(std::shared_ptr<zmq::context_t>(&external, [](zmq::context_t *) {}))
  {
  }

  // Copyable and movable.
  Context(const Context &) = default;
  Context(Context &&) noexcept = default;
  Context & operator=(const Context &) = default;
  Context & operator=(Context &&) noexcept = default;
  ~Context() = default;

  zmq::context_t & raw() noexcept {return *ctx_;}
  const zmq::context_t & raw() const noexcept {return *ctx_;}

private:
  std::shared_ptr<zmq::context_t> ctx_;
};

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__CONTEXT_HPP_
