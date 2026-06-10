#include "goblin/storage/s3fifo.hpp"

#include <algorithm>

namespace goblin::storage {

S3Fifo::S3Fifo(std::size_t capacity_hint) : cap_(capacity_hint == 0 ? 1 : capacity_hint) {}

void S3Fifo::insert(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end()) {
        it->second.visited = true; // already cached -> treat as a hit
        return;
    }
    if (ghost_set_.erase(d) > 0) { // recently evicted -> straight to main
        meta_.emplace(d, Entry{Q::main, false});
        main_.push_back(d);
        ++main_n_;
    } else {
        meta_.emplace(d, Entry{Q::small, false});
        small_.push_back(d);
        ++small_n_;
    }
}

void S3Fifo::touch(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end()) it->second.visited = true;
}

void S3Fifo::remove(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end()) {
        if (it->second.q == Q::small)
            --small_n_;
        else
            --main_n_;
        meta_.erase(it);
    }
    ghost_set_.erase(d);
}

std::optional<Digest> S3Fifo::pop_front_resident(std::deque<Digest>& q, Q which) {
    while (!q.empty()) {
        const Digest d = q.front();
        q.pop_front();
        const auto it = meta_.find(d);
        if (it != meta_.end() && it->second.q == which) return d;
        // else stale (removed, or promoted to the other queue) -> skip
    }
    return std::nullopt;
}

void S3Fifo::ghost_push(const Digest& d) {
    ghost_.push_back(d);
    ghost_set_.insert(d);
    while (ghost_.size() > cap_) {
        ghost_set_.erase(ghost_.front());
        ghost_.pop_front();
    }
}

std::optional<Digest> S3Fifo::evict() {
    if (small_n_ == 0 && main_n_ == 0) return std::nullopt;

    // Decide which queue to drain (once). Empty queues force the other.
    bool from_small;
    if (small_n_ == 0)
        from_small = false;
    else if (main_n_ == 0)
        from_small = true;
    else
        from_small = small_n_ >= std::max<std::size_t>(1, cap_ / 10);

    if (from_small) {
        // Promote accessed items to main; evict the first un-accessed one (to ghost).
        while (auto d = pop_front_resident(small_, Q::small)) {
            --small_n_;
            const auto it = meta_.find(*d);
            if (it->second.visited) {
                it->second = Entry{Q::main, false};
                main_.push_back(*d);
                ++main_n_;
                continue;
            }
            meta_.erase(it);
            ghost_push(*d);
            return d;
        }
        // small drained (all promoted) -> fall through to main
    }

    // Main: give accessed items a second chance; evict the first un-accessed one.
    while (auto d = pop_front_resident(main_, Q::main)) {
        const auto it = meta_.find(*d);
        if (it->second.visited) {
            it->second.visited = false;
            main_.push_back(*d);
            continue;
        }
        --main_n_;
        meta_.erase(it);
        return d;
    }
    return std::nullopt;
}

std::optional<bool> S3Fifo::in_main(const Digest& d) const {
    const auto it = meta_.find(d);
    if (it == meta_.end()) return std::nullopt;
    return it->second.q == Q::main;
}

} // namespace goblin::storage
