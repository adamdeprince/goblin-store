#include "goblin/storage/striped_io.hpp"

#include <unistd.h> // pwrite
#include <vector>

namespace goblin::storage {

Status striped_pwrite(const DrivePool& pool, std::uint64_t key_hash, std::span<const int> fds,
                      Offset offset, ByteView data) {
    const auto segs = pool.plan_reads(key_hash, offset, data.size());
    Size dst = 0;
    for (const auto& s : segs) {
        const std::byte* src = data.data() + dst;
        Size left = s.length;
        Offset fo = s.file_offset;
        while (left > 0) {
            const ssize_t w = ::pwrite(fds[s.drive], src, left, static_cast<off_t>(fo));
            if (w <= 0) return err(Errc::io_error, "pwrite failed");
            src += w;
            fo += static_cast<Size>(w);
            left -= static_cast<Size>(w);
        }
        dst += s.length;
    }
    return {};
}

Result<std::size_t> striped_read(core::Reactor& reactor, const DrivePool& pool,
                                 std::uint64_t key_hash, std::span<const int> fds, Offset offset,
                                 MutBytes out) {
    const auto segs = pool.plan_reads(key_hash, offset, out.size());
    if (segs.empty()) return std::size_t{0};

    // Queue one read per chunk; the kernel reads each into its (contiguous) slice of `out`.
    Size dst = 0;
    for (const auto& s : segs) {
        if (!reactor.submit_read(fds[s.drive], s.file_offset, out.subspan(dst, s.length), 0)) {
            reactor.submit(); // SQ full: flush and retry once (TODO: batch for huge reads)
            if (!reactor.submit_read(fds[s.drive], s.file_offset, out.subspan(dst, s.length), 0))
                return err(Errc::io_error, "submission queue full");
        }
        dst += s.length;
    }

    const unsigned n = static_cast<unsigned>(segs.size());
    reactor.submit_and_wait(n);

    std::vector<core::Completion> comps(n);
    std::size_t total = 0;
    unsigned got = 0;
    while (got < n) {
        const unsigned r = reactor.reap(std::span<core::Completion>(comps.data(), n));
        if (r == 0) {
            reactor.submit_and_wait(1);
            continue;
        }
        for (unsigned k = 0; k < r; ++k) {
            if (comps[k].res < 0) return err(Errc::io_error, "striped read failed");
            total += static_cast<std::size_t>(comps[k].res);
        }
        got += r;
    }
    return total;
}

} // namespace goblin::storage
