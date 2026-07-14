#include "goblin/core/reactor.hpp"

#if GOBLIN_HAVE_URING

#include <cstring> // std::strerror
#include <string>
#include <sys/socket.h> // MSG_NOSIGNAL

namespace goblin::core {

Result<Reactor> Reactor::create(unsigned entries) {
    auto ring = RingPtr(new io_uring());
    const int r = io_uring_queue_init(entries, ring.get(), 0);
    if (r < 0)
        return err(Errc::io_error, std::string("io_uring_queue_init: ") + std::strerror(-r));
    return Reactor(std::move(ring));
}

bool Reactor::submit_read(int fd, std::uint64_t offset, MutBytes buf, std::uint64_t user_data) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.get());
    if (!sqe) return false; // SQ full
    io_uring_prep_read(sqe, fd, buf.data(), static_cast<unsigned>(buf.size()), offset);
    sqe->user_data = user_data; // the stable field works with liburing 2.0 and newer
    return true;
}

bool Reactor::submit_recv(int fd, MutBytes buf, std::uint64_t user_data) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.get());
    if (!sqe) return false;
    io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), 0);
    sqe->user_data = user_data;
    return true;
}

bool Reactor::submit_send(int fd, ByteView buf, std::uint64_t user_data) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.get());
    if (!sqe) return false;
    io_uring_prep_send(sqe, fd, buf.data(), buf.size(), MSG_NOSIGNAL);
    sqe->user_data = user_data;
    return true;
}

bool Reactor::submit_accept(int listen_fd, std::uint64_t user_data) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.get());
    if (!sqe) return false;
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    sqe->user_data = user_data;
    return true;
}

bool Reactor::submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring_.get());
    if (!sqe) return false;
    io_uring_prep_poll_add(sqe, fd, poll_mask);
    sqe->user_data = user_data;
    return true;
}

int Reactor::submit() {
    const int n = io_uring_submit(ring_.get());
    return n < 0 ? 0 : n;
}

int Reactor::submit_and_wait(unsigned min_complete) {
    return io_uring_submit_and_wait(ring_.get(), min_complete);
}

void Reactor::submit_and_wait_timeout(unsigned timeout_ms) {
    io_uring_submit(ring_.get());
    __kernel_timespec ts{};
    ts.tv_sec = static_cast<long long>(timeout_ms / 1000);
    ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1'000'000;
    io_uring_cqe* cqe = nullptr;
    io_uring_wait_cqe_timeout(ring_.get(), &cqe, &ts); // -ETIME on timeout; caller reaps what's ready
}

unsigned Reactor::reap(std::span<Completion> out) {
    unsigned n = 0;
    io_uring_cqe* cqe = nullptr;
    while (n < out.size() && io_uring_peek_cqe(ring_.get(), &cqe) == 0) {
        const std::uint64_t ud = cqe->user_data;
        const int res = cqe->res;
        io_uring_cqe_seen(ring_.get(), cqe);
        if (ud == LIBURING_UDATA_TIMEOUT) continue; // internal marker from wait_cqe_timeout
        out[n].user_data = ud;
        out[n].res = res;
        ++n;
    }
    return n;
}

Result<std::size_t> Reactor::read_sync(int fd, std::uint64_t offset, MutBytes buf) {
    if (!submit_read(fd, offset, buf, 0)) return err(Errc::io_error, "submission queue full");
    const int s = io_uring_submit(ring_.get());
    if (s < 0) return err(Errc::io_error, std::string("io_uring_submit: ") + std::strerror(-s));

    io_uring_cqe* cqe = nullptr;
    const int w = io_uring_wait_cqe(ring_.get(), &cqe);
    if (w < 0) return err(Errc::io_error, std::string("io_uring_wait_cqe: ") + std::strerror(-w));
    const int res = cqe->res;
    io_uring_cqe_seen(ring_.get(), cqe);
    if (res < 0) return err(Errc::io_error, std::string("read: ") + std::strerror(-res));
    return static_cast<std::size_t>(res);
}

} // namespace goblin::core

#else // !GOBLIN_HAVE_URING — stub (built without liburing)

namespace goblin::core {

Result<Reactor> Reactor::create(unsigned) {
    return err(Errc::unsupported, "built without liburing (GOBLIN_HAVE_URING=0)");
}
bool Reactor::submit_read(int, std::uint64_t, MutBytes, std::uint64_t) { return false; }
bool Reactor::submit_recv(int, MutBytes, std::uint64_t) { return false; }
bool Reactor::submit_send(int, ByteView, std::uint64_t) { return false; }
bool Reactor::submit_accept(int, std::uint64_t) { return false; }
bool Reactor::submit_poll(int, unsigned, std::uint64_t) { return false; }
int Reactor::submit() { return 0; }
int Reactor::submit_and_wait(unsigned) { return 0; }
void Reactor::submit_and_wait_timeout(unsigned) {}
unsigned Reactor::reap(std::span<Completion>) { return 0; }
Result<std::size_t> Reactor::read_sync(int, std::uint64_t, MutBytes) {
    return err(Errc::unsupported, "built without liburing");
}

} // namespace goblin::core

#endif
