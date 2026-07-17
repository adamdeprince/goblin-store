#include "goblin/net/stream_io.hpp"

#include <unistd.h>

namespace goblin::net {

bool UringStreamIo::submit_read(int fd, std::uint64_t offset, MutBytes buf,
                                std::uint64_t user_data) {
    return reactor_.submit_read(fd, offset, buf, user_data);
}
bool UringStreamIo::submit_recv(int fd, MutBytes buf, std::uint64_t user_data) {
    return reactor_.submit_recv(fd, buf, user_data);
}
bool UringStreamIo::submit_send(int fd, ByteView buf, std::uint64_t user_data, int flags) {
    return reactor_.submit_send(fd, buf, user_data, flags);
}
bool UringStreamIo::submit_sendmsg(int fd, msghdr* msg, std::uint64_t user_data, int flags) {
    return reactor_.submit_sendmsg(fd, msg, user_data, flags);
}
bool UringStreamIo::submit_accept(int listen_fd, std::uint64_t user_data) {
    return reactor_.submit_accept(listen_fd, user_data);
}
bool UringStreamIo::submit_poll(int fd, unsigned poll_mask, std::uint64_t user_data) {
    return reactor_.submit_poll(fd, poll_mask, user_data);
}
unsigned UringStreamIo::submission_space() const noexcept { return reactor_.submission_space(); }
int UringStreamIo::submit() { return reactor_.submit(); }
void UringStreamIo::submit_and_wait_timeout(unsigned timeout_ms) {
    reactor_.submit_and_wait_timeout(timeout_ms);
}
unsigned UringStreamIo::reap(std::span<core::Completion> out) { return reactor_.reap(out); }
void UringStreamIo::close_fd(int fd) noexcept { ::close(fd); }

} // namespace goblin::net
