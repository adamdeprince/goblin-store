#include "goblin/storage/s3fifo.hpp"

#include <algorithm>

namespace goblin::storage {

S3Fifo::S3Fifo(std::size_t capacity_hint) : cap_(capacity_hint == 0 ? 1 : capacity_hint) {}

void S3Fifo::insert(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end()) {
        it->second.visited.store(true, std::memory_order_relaxed);
        return;
    }
    const std::uint64_t ticket = next_ticket();
    if (ghost_tickets_.erase(d) > 0) { // recently evicted -> straight to main
        meta_.try_emplace(d, Q::main, false, ticket);
        main_.push_back(QueueNode{d, ticket});
        ++main_n_;
    } else {
        meta_.try_emplace(d, Q::small, false, ticket);
        small_.push_back(QueueNode{d, ticket});
        ++small_n_;
    }
}

void S3Fifo::touch(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end())
        it->second.visited.store(true, std::memory_order_relaxed);
}

void S3Fifo::remove(const Digest& d) {
    if (const auto it = meta_.find(d); it != meta_.end()) {
        if (it->second.q == Q::small)
            --small_n_;
        else
            --main_n_;
        meta_.erase(it);
    }
    ghost_tickets_.erase(d);
}

std::optional<Digest> S3Fifo::pop_front_resident(std::deque<QueueNode>& q, Q which) {
    while (!q.empty()) {
        const QueueNode node = q.front();
        q.pop_front();
        const auto it = meta_.find(node.digest);
        if (it != meta_.end() && it->second.q == which && it->second.ticket == node.ticket)
            return node.digest;
        // Else stale: removed, promoted, or an older incarnation of the same digest.
    }
    return std::nullopt;
}

void S3Fifo::ghost_push(const Digest& d, std::uint64_t ticket) {
    ghost_.push_back(QueueNode{d, ticket});
    ghost_tickets_.insert_or_assign(d, ticket);
    while (ghost_.size() > cap_) {
        const QueueNode old = ghost_.front();
        ghost_.pop_front();
        const auto current = ghost_tickets_.find(old.digest);
        if (current != ghost_tickets_.end() && current->second == old.ticket)
            ghost_tickets_.erase(current);
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
            if (it->second.visited.exchange(false, std::memory_order_relaxed)) {
                it->second.q = Q::main;
                main_.push_back(QueueNode{*d, it->second.ticket});
                ++main_n_;
                continue;
            }
            const std::uint64_t ticket = it->second.ticket;
            meta_.erase(it);
            ghost_push(*d, ticket);
            return d;
        }
        // small drained (all promoted) -> fall through to main
    }

    // Main: give accessed items a second chance; evict the first un-accessed one.
    while (auto d = pop_front_resident(main_, Q::main)) {
        const auto it = meta_.find(*d);
        if (it->second.visited.exchange(false, std::memory_order_relaxed)) {
            main_.push_back(QueueNode{*d, it->second.ticket});
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
