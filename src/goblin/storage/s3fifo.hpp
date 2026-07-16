// S3-FIFO eviction policy (Yang et al., SOSP'23): a small + main FIFO queue with a ghost queue.
// Lock-light — a hit just sets a visited bit (no list surgery). The caller drives eviction: when
// it needs room (e.g. the RAM head pool is full) it calls evict() for the next victim and frees
// it. `capacity_hint` sizes the small queue (~10%) and bounds the ghost queue; the hard capacity
// is the caller's (the pool's bytes). v1 is single-threaded (no internal locking).
//
// First concrete eviction policy (ADR-0007); SIEVE / W-TinyLFU follow behind a shared interface.
#pragma once

#include "goblin/crypto/sha256.hpp"
#include "goblin/storage/eviction.hpp" // EvictionPolicy
#include "goblin/storage/index.hpp"    // Digest, DigestHash

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace goblin::storage {

class S3Fifo final : public EvictionPolicy {
public:
    explicit S3Fifo(std::size_t capacity_hint);

    void insert(const Digest&) override; // -> small, or main if seen recently via ghost
    void touch(const Digest&) override;  // hit: lazy-promote (set visited)
    void remove(const Digest&) override; // dropped externally (e.g. DELETE)
    std::optional<Digest> evict() override; // next victim, removed; nullopt if nothing resident

    std::size_t resident() const noexcept override { return meta_.size(); }
    bool contains(const Digest& d) const { return meta_.contains(d); }
    std::optional<bool> in_main(const Digest& d) const; // introspection / tests

private:
    enum class Q : std::uint8_t { small, main };
    struct QueueNode {
        Digest digest;
        std::uint64_t ticket;
    };
    struct Entry {
        Q q;
        bool visited;
        std::uint64_t ticket;
    };
    std::optional<Digest> pop_front_resident(std::deque<QueueNode>& q, Q which);
    void ghost_push(const Digest&, std::uint64_t ticket);
    std::uint64_t next_ticket() noexcept { return ++next_ticket_; }

    std::deque<QueueNode> small_, main_, ghost_;
    std::unordered_map<Digest, Entry, DigestHash> meta_; // resident items only
    // A ghost deque node removes membership only if its ticket is still current. Without the ticket,
    // trimming an old occurrence could erase a newer ghost incarnation of the same digest.
    std::unordered_map<Digest, std::uint64_t, DigestHash> ghost_tickets_;
    std::size_t cap_;
    std::size_t small_n_ = 0; // resident count in the small queue
    std::size_t main_n_ = 0;  // resident count in the main queue
    std::uint64_t next_ticket_ = 0; // fresh identity for each true admission (wrap is unreachable)
};

} // namespace goblin::storage
