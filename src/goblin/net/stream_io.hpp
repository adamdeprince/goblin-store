// Completion-shaped stream I/O used by the shared memcache/HTTP state machine.
//
// The ordinary adapter forwards network and disk operations to one io_uring Reactor.  The
// readiness adapter keeps disk reads on io_uring while issuing socket operations through the
// standard nonblocking API, allowing ExaSock's preload library to see and accelerate them.
#pragma once

#include "goblin/common/error.hpp"
#include "goblin/common/types.hpp"
#include "goblin/core/reactor.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <sys/socket.h>

namespace goblin::net {

class StreamIo {
public:
    virtual ~StreamIo() = default;

    StreamIo(const StreamIo&) = delete;
    StreamIo& operator=(const StreamIo&) = delete;

    virtual bool submit_read(int fd, std::uint64_t offset, MutBytes buf,
                             std::uint64_t user_data) = 0;
    virtual bool submit_recv(int fd, MutBytes buf, std::uint64_t user_data) = 0;
    virtual bool submit_send(int fd, ByteView buf, std::uint64_t user_data,
                             int flags = 0) = 0;
    virtual bool submit_sendmsg(int fd, msghdr* msg, std::uint64_t user_data,
                                int flags = 0) = 0;
    virtual bool submit_accept(int listen_fd, std::uint64_t user_data) = 0;
    virtual bool submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data) = 0;

    virtual unsigned submission_space() const noexcept = 0;
    virtual int submit() = 0;
    virtual void submit_and_wait_timeout(unsigned timeout_ms) = 0;
    virtual unsigned reap(std::span<core::Completion> out) = 0;

    // Close a stream descriptor and arrange cancellation completions for any readiness operation
    // that was waiting on it.  This preserves StreamLoop's in-flight lifetime accounting.
    virtual void close_fd(int fd) noexcept = 0;

protected:
    StreamIo() = default;
};

class UringStreamIo final : public StreamIo {
public:
    explicit UringStreamIo(core::Reactor& reactor) noexcept : reactor_(reactor) {}

    bool submit_read(int fd, std::uint64_t offset, MutBytes buf,
                     std::uint64_t user_data) override;
    bool submit_recv(int fd, MutBytes buf, std::uint64_t user_data) override;
    bool submit_send(int fd, ByteView buf, std::uint64_t user_data, int flags) override;
    bool submit_sendmsg(int fd, msghdr* msg, std::uint64_t user_data, int flags) override;
    bool submit_accept(int listen_fd, std::uint64_t user_data) override;
    bool submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data) override;
    unsigned submission_space() const noexcept override;
    int submit() override;
    void submit_and_wait_timeout(unsigned timeout_ms) override;
    unsigned reap(std::span<core::Completion> out) override;
    void close_fd(int fd) noexcept override;

private:
    core::Reactor& reactor_;
};

// Hardware-free readiness driver.  On Linux this uses epoll for standard nonblocking socket calls
// and an eventfd for the supplied reactor's disk completions.  It is available independently of
// the ExaSock build flag so its completion/cancellation semantics can be tested without hardware.
Result<std::unique_ptr<StreamIo>> make_readiness_stream_io(
    core::Reactor& disk_reactor, bool require_exasock_connections = false);

// Explicit per-socket ExaSock opt-in.  With the supported `exasock --no-auto` launch mode, zeroing
// SO_EXA_NO_ACCEL enables acceleration only for the selected listener.  The stub returns
// unsupported when GOBLIN_ENABLE_EXASOCK was not requested.
Status enable_exasock_socket(int fd);
constexpr bool exasock_compiled() noexcept {
#if GOBLIN_HAVE_EXASOCK
    return true;
#else
    return false;
#endif
}

} // namespace goblin::net
