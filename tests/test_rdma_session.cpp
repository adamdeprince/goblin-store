#include "mini_test.hpp"

#include "goblin/core/stats.hpp"
#include "goblin/crypto/sha256.hpp"
#include "goblin/protocol/memcache/rdma_session.hpp"
#include "goblin/storage/index.hpp"
#include "goblin/storage/tier_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace goblin;
using namespace goblin::memcache;

namespace {

ByteView bytes_of(std::string_view s) {
    return ByteView(reinterpret_cast<const std::byte*>(s.data()), s.size());
}

std::string string_of(ByteView bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class FakeRdmaChannel final : public RdmaSessionChannel {
public:
    enum class WindowState { free, reserved, inflight };

    struct Window {
        std::vector<std::byte> storage;
        WindowState state = WindowState::free;
    };

    struct RxRecord {
        std::string data;
        bool consumed = false;
        bool released = false;
        unsigned attempts = 0;
    };

    struct BulkSend {
        unsigned id = 0;
        std::string data;
    };

    explicit FakeRdmaChannel(std::size_t window_bytes, unsigned window_count = 1,
                             std::size_t inline_bytes = 192)
        : inline_bytes_(inline_bytes) {
        windows_.reserve(window_count);
        for (unsigned i = 0; i < window_count; ++i)
            windows_.push_back(Window{std::vector<std::byte>(window_bytes)});
    }

    std::size_t inline_capacity() const noexcept override { return inline_bytes_; }

    bool try_send_inline(std::string_view bytes) noexcept override {
        if (block_inline_) {
            events.push_back("tx-inline-blocked");
            return false;
        }
        CHECK(bytes.size() <= inline_bytes_);
        inline_stream.append(bytes);
        events.push_back("tx-inline:" + std::string(bytes));
        return true;
    }

    std::optional<TxWindow> acquire_tx_window() noexcept override {
        for (unsigned i = 0; i < windows_.size(); ++i) {
            if (windows_[i].state != WindowState::free) continue;
            windows_[i].state = WindowState::reserved;
            ++acquire_count;
            events.push_back("tx-acquire:" + std::to_string(i));
            return TxWindow{i, MutBytes(windows_[i].storage.data(), windows_[i].storage.size())};
        }
        return std::nullopt;
    }

    bool try_send_tx_window(unsigned id, std::size_t length) noexcept override {
        CHECK(id < windows_.size());
        if (id >= windows_.size()) return false;
        CHECK(windows_[id].state == WindowState::reserved);
        CHECK(length <= windows_[id].storage.size());
        if (block_bulk_) {
            events.push_back("tx-bulk-blocked:" + std::to_string(id));
            return false;
        }
        windows_[id].state = WindowState::inflight;
        const ByteView data(windows_[id].storage.data(), length);
        bulk_sends.push_back(BulkSend{id, string_of(data)});
        events.push_back("tx-bulk:" + std::to_string(id) + ":" + string_of(data));
        return true;
    }

    void release_tx_window(unsigned id) noexcept override {
        CHECK(id < windows_.size());
        if (id >= windows_.size()) return;
        CHECK(windows_[id].state == WindowState::reserved);
        windows_[id].state = WindowState::free;
        events.push_back("tx-cancel:" + std::to_string(id));
    }

    unsigned offer_rx(std::string data) {
        CHECK(data.size() <= rx_window_bytes());
        const unsigned id = static_cast<unsigned>(rx_records.size());
        rx_records.push_back(RxRecord{std::move(data)});
        events.push_back("rx-ready:" + std::to_string(id));
        return id;
    }

    RdmaSession::ConsumeResult consume_rx(RdmaSession& session, unsigned id) {
        CHECK(id < rx_records.size());
        if (id >= rx_records.size()) return RdmaSession::ConsumeResult::fatal;
        RxRecord& record = rx_records[id];
        CHECK(!record.consumed);
        CHECK(!record.released);
        ++record.attempts;
        events.push_back("rx-attempt:" + std::to_string(id));
        const auto result = session.accept_bulk(bytes_of(record.data));
        if (result == RdmaSession::ConsumeResult::consumed) {
            record.consumed = true;
            events.push_back("rx-consumed-after-write:" + std::to_string(id));
        } else if (result == RdmaSession::ConsumeResult::blocked) {
            events.push_back("rx-retained:" + std::to_string(id));
        } else {
            events.push_back("rx-fatal:" + std::to_string(id));
        }
        return result;
    }

    void release_rx(unsigned id) {
        CHECK(id < rx_records.size());
        if (id >= rx_records.size()) return;
        CHECK(rx_records[id].consumed);
        CHECK(!rx_records[id].released);
        rx_records[id].released = true;
        events.push_back("rx-release:" + std::to_string(id));
    }

    void complete_tx(unsigned id) {
        CHECK(id < windows_.size());
        if (id >= windows_.size()) return;
        CHECK(windows_[id].state == WindowState::inflight);
        windows_[id].state = WindowState::free;
        events.push_back("tx-complete:" + std::to_string(id));
    }

    std::size_t rx_window_bytes() const {
        return windows_.empty() ? 0 : windows_.front().storage.size();
    }

    WindowState window_state(unsigned id) const { return windows_.at(id).state; }

    void reset_observations() {
        events.clear();
        inline_stream.clear();
        bulk_sends.clear();
        acquire_count = 0;
    }

    void set_bulk_blocked(bool blocked) { block_bulk_ = blocked; }

    std::vector<std::string> events;
    std::string inline_stream;
    std::vector<BulkSend> bulk_sends;
    std::vector<RxRecord> rx_records;
    unsigned acquire_count = 0;

private:
    std::size_t inline_bytes_;
    std::vector<Window> windows_;
    bool block_inline_ = false;
    bool block_bulk_ = false;
};

class SessionFixture {
private:
    static fs::path make_root() {
        static std::atomic<unsigned> sequence{0};
        return fs::temp_directory_path() /
               ("goblin-rdma-session-" + std::to_string(::getpid()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    }

    fs::path root_;

public:
    explicit SessionFixture(std::size_t window_bytes = 4, unsigned tx_windows = 1,
                            Size ram_head = 64, unsigned write_buffers = 1)
        : root_(make_root()), channel(window_bytes, tx_windows) {
        fs::create_directories(root_ / "ssd");

        TierSizes tiers;
        tiers.ram_head = ram_head;
        tiers.ssd_prefix = 1 * MiB;
        MemoryConfig memory;
        memory.total_bytes = 256 * KiB;
        memory.block_bytes = 64 * KiB;
        memory.lock_memory = false;
        memory.use_hugepages = false;
        EvictionConfig eviction;
        PoolConfig ssd;
        ssd.dirs.push_back((root_ / "ssd").string());
        ssd.stripe_unit = 4 * KiB;
        PoolConfig hdd;

        auto opened = storage::TierManager::open(tiers, memory, eviction, ssd, hdd, index,
                                                 4 * KiB, write_buffers, false);
        CHECK(opened.has_value());
        if (opened) {
            tm.emplace(std::move(*opened));
            session.emplace(*tm, index, nullptr, stats, nullptr, channel);
        }
    }

    ~SessionFixture() {
        session.reset();
        tm.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    bool ready() const { return session.has_value(); }

    storage::Index index;
    std::optional<storage::TierManager> tm;
    core::Stats stats;
    FakeRdmaChannel channel;
    std::optional<RdmaSession> session;
};

bool accept_inline(SessionFixture& f, std::string_view data) {
    return f.session->accept_inline(data) == RdmaSession::ConsumeResult::consumed;
}

bool accept_and_release(SessionFixture& f, unsigned id) {
    if (f.channel.consume_rx(*f.session, id) != RdmaSession::ConsumeResult::consumed)
        return false;
    f.channel.release_rx(id);
    return true;
}

std::string sent_bulk(const FakeRdmaChannel& channel) {
    std::string result;
    for (const auto& send : channel.bulk_sends) result += send.data;
    return result;
}

std::size_t event_index(const FakeRdmaChannel& channel, std::string_view event) {
    const auto found = std::find(channel.events.begin(), channel.events.end(), event);
    return found == channel.events.end()
        ? channel.events.size()
        : static_cast<std::size_t>(found - channel.events.begin());
}

} // namespace

TEST("rdma session: tiny SET body is consumed from bulk and released only after write") {
    SessionFixture f;
    if (!f.ready()) return;

    CHECK(accept_inline(f, "set tiny 7 0 1\r\n"));
    const unsigned rx = f.channel.offer_rx("x");
    CHECK(f.channel.consume_rx(*f.session, rx) == RdmaSession::ConsumeResult::consumed);

    // The session increments this boundary only after StoreHandle::write returned. The transport
    // has not yet published RELEASE, so the registered source is still owned by the receiver.
    CHECK_EQ(f.session->lifetime_counters().bulk_records_received, std::uint64_t(1));
    CHECK_EQ(f.session->lifetime_counters().bulk_records_released_after_write, std::uint64_t(1));
    CHECK(!f.channel.rx_records[rx].released);
    f.channel.release_rx(rx);
    CHECK(event_index(f.channel, "rx-consumed-after-write:0") <
          event_index(f.channel, "rx-release:0"));

    CHECK(accept_inline(f, "\r\n"));
    CHECK(f.session->drive());
    CHECK_EQ(f.channel.inline_stream, std::string("STORED\r\n"));
    const auto meta = f.index.lookup(crypto::hash_key("tiny"));
    CHECK(meta.has_value());
    if (meta) {
        CHECK_EQ(meta->flags, std::uint32_t(7));
        CHECK_EQ(meta->size, Size(1));
    }
    CHECK_EQ(f.stats.bytes_stored.load(std::memory_order_relaxed), std::uint64_t(1));
}

TEST("rdma session: SET larger than one window is a sequence of bulk records") {
    SessionFixture f(/*window_bytes=*/4);
    if (!f.ready()) return;

    CHECK(accept_inline(f, "set wide 0 0 9\r\n"));
    for (const std::string_view piece : {"abcd", "efgh", "i"}) {
        const unsigned rx = f.channel.offer_rx(std::string(piece));
        CHECK(accept_and_release(f, rx));
    }
    CHECK_EQ(f.session->lifetime_counters().bulk_records_received, std::uint64_t(3));
    CHECK_EQ(f.session->lifetime_counters().bulk_records_released_after_write, std::uint64_t(3));
    CHECK(accept_inline(f, "\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("STORED\r\n"));

    const auto head = f.tm->head_view(crypto::hash_key("wide"));
    CHECK(head.has_value());
    if (head) CHECK_EQ(string_of(*head), std::string("abcdefghi"));
}

TEST("rdma session: rejected ADD drains and releases its bulk body without storing it") {
    SessionFixture f(/*window_bytes=*/4);
    if (!f.ready()) return;
    CHECK(f.tm->store(crypto::hash_key("same"), bytes_of("old"), 9).has_value());

    CHECK(accept_inline(f, "add same 0 0 11\r\n"));
    for (const std::string_view piece : {"repl", "acem", "ent"}) {
        const unsigned rx = f.channel.offer_rx(std::string(piece));
        CHECK(accept_and_release(f, rx));
    }
    CHECK_EQ(f.session->lifetime_counters().bulk_records_received, std::uint64_t(3));
    CHECK_EQ(f.session->lifetime_counters().bulk_records_released_after_write, std::uint64_t(3));
    CHECK(std::all_of(f.channel.rx_records.begin(), f.channel.rx_records.end(),
                      [](const FakeRdmaChannel::RxRecord& record) { return record.released; }));
    CHECK_EQ(f.stats.bytes_stored.load(std::memory_order_relaxed), std::uint64_t(0));

    CHECK(accept_inline(f, "\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("NOT_STORED\r\n"));
    CHECK_EQ(f.stats.set_rejected.load(std::memory_order_relaxed), std::uint64_t(1));
    const auto meta = f.index.lookup(crypto::hash_key("same"));
    CHECK(meta.has_value());
    if (meta) {
        CHECK_EQ(meta->flags, std::uint32_t(9));
        CHECK_EQ(meta->size, Size(3));
    }
    const auto head = f.tm->head_view(crypto::hash_key("same"));
    CHECK(head.has_value());
    if (head) CHECK_EQ(string_of(*head), std::string("old"));

    // A subsequent control command proves the discarded body and trailer preserved framing.
    f.channel.reset_observations();
    CHECK(accept_inline(f, "version\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("VERSION goblin-store 0.0.2\r\n"));
}

TEST("rdma session: inline SET body is a fatal v3 framing error") {
    SessionFixture f;
    if (!f.ready()) return;

    CHECK(accept_inline(f, "set wrong 0 0 1\r\n"));
    CHECK(f.session->accept_inline("x") == RdmaSession::ConsumeResult::fatal);
    CHECK(f.session->failed());
    CHECK(f.session->error().find("arrived inline") != std::string_view::npos);
    CHECK(!f.index.lookup(crypto::hash_key("wrong")).has_value());
}

TEST("rdma session: blocked SET retains the same RX record until a write buffer is free") {
    SessionFixture f(/*window_bytes=*/128, /*tx_windows=*/1, /*ram_head=*/64,
                     /*write_buffers=*/1);
    if (!f.ready()) return;

    // A disk-backed writer owns the sole staging block, forcing the session into set_wait.
    auto held = f.tm->begin_store(crypto::hash_key("holder"), 65);
    CHECK(held.has_value());
    if (!held) return;
    std::optional<storage::TierManager::StoreHandle> blocker;
    blocker.emplace(std::move(*held));

    CHECK(accept_inline(f, "set waiting 0 0 65\r\n"));
    const unsigned rx = f.channel.offer_rx(std::string(65, 'w'));
    CHECK(f.channel.consume_rx(*f.session, rx) == RdmaSession::ConsumeResult::blocked);
    CHECK(!f.channel.rx_records[rx].released);
    CHECK_EQ(f.channel.rx_records[rx].attempts, 1u);
    CHECK_EQ(f.session->lifetime_counters().bulk_records_received, std::uint64_t(0));
    CHECK(!f.session->drive()); // the same storage resource is still unavailable

    blocker.reset();
    CHECK(f.session->drive()); // retry_store obtains the staging block; RX ownership is unchanged
    CHECK(f.channel.consume_rx(*f.session, rx) == RdmaSession::ConsumeResult::consumed);
    CHECK_EQ(f.channel.rx_records[rx].attempts, 2u);
    CHECK(!f.channel.rx_records[rx].released);
    f.channel.release_rx(rx);
    CHECK(accept_inline(f, "\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("STORED\r\n"));
    CHECK(f.index.lookup(crypto::hash_key("waiting")).has_value());
}

TEST("rdma session: GET sends inline header, only bulk body, then inline trailer") {
    SessionFixture f(/*window_bytes=*/4);
    if (!f.ready()) return;
    const std::string value = "abcdefghi";
    CHECK(f.tm->store(crypto::hash_key("wide"), bytes_of(value), 5).has_value());

    f.channel.set_bulk_blocked(true);
    CHECK(accept_inline(f, "get wide\r\n"));
    CHECK(f.session->drive());
    CHECK_EQ(f.channel.inline_stream, std::string("VALUE wide 5 9\r\n"));
    CHECK_EQ(f.channel.acquire_count, 1u);
    CHECK(f.channel.bulk_sends.empty());
    CHECK(f.channel.window_state(0) == FakeRdmaChannel::WindowState::reserved);

    // Backpressure must retain the prepared window and its bytes; retry does not reacquire/copy it.
    CHECK(!f.session->drive());
    CHECK_EQ(f.channel.acquire_count, 1u);
    f.channel.set_bulk_blocked(false);
    CHECK(f.session->drive());
    CHECK_EQ(f.channel.acquire_count, 1u);
    CHECK_EQ(f.channel.bulk_sends.size(), std::size_t(1));
    CHECK_EQ(f.channel.bulk_sends[0].data, std::string("abcd"));

    f.channel.complete_tx(0);
    CHECK(f.session->drive());
    CHECK_EQ(f.channel.bulk_sends.size(), std::size_t(2));
    CHECK_EQ(f.channel.inline_stream, std::string("VALUE wide 5 9\r\n"));
    f.channel.complete_tx(0);
    CHECK(f.session->drive());

    CHECK_EQ(sent_bulk(f.channel), value);
    CHECK_EQ(f.channel.bulk_sends.size(), std::size_t(3));
    CHECK_EQ(f.channel.inline_stream, std::string("VALUE wide 5 9\r\n\r\nEND\r\n"));
    CHECK_EQ(f.session->lifetime_counters().bulk_records_sent, std::uint64_t(3));
    CHECK(event_index(f.channel, "tx-inline:VALUE wide 5 9\r\n") <
          event_index(f.channel, "tx-bulk:0:abcd"));
    CHECK(event_index(f.channel, "tx-bulk:0:i") <
          event_index(f.channel, "tx-inline:\r\nEND\r\n"));
}

TEST("rdma session: zero-byte SET and GET use no bulk records") {
    SessionFixture f;
    if (!f.ready()) return;

    CHECK(accept_inline(f, "set empty 0 0 0\r\n"));
    CHECK(accept_inline(f, "\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("STORED\r\n"));
    CHECK(f.channel.bulk_sends.empty());
    CHECK_EQ(f.session->lifetime_counters().bulk_records_received, std::uint64_t(0));

    f.channel.reset_observations();
    CHECK(accept_inline(f, "get empty\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("VALUE empty 0 0\r\n\r\nEND\r\n"));
    CHECK(f.channel.bulk_sends.empty());
    CHECK_EQ(f.session->lifetime_counters().bulk_records_sent, std::uint64_t(0));
}

TEST("rdma session: version, delete, malformed command, and stats remain inline control") {
    SessionFixture f(/*window_bytes=*/4, /*tx_windows=*/1, /*ram_head=*/64);
    if (!f.ready()) return;

    CHECK(accept_inline(f, "version\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("VERSION goblin-store 0.0.2\r\n"));

    f.channel.reset_observations();
    CHECK(accept_inline(f, "set doomed 0 0 1\r\n"));
    const unsigned rx = f.channel.offer_rx("d");
    CHECK(accept_and_release(f, rx));
    CHECK(accept_inline(f, "\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("STORED\r\n"));

    f.channel.reset_observations();
    CHECK(accept_inline(f, "delete doomed\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("DELETED\r\n"));
    CHECK(!f.index.lookup(crypto::hash_key("doomed")).has_value());

    f.channel.reset_observations();
    CHECK(accept_inline(f, "delete doomed\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("NOT_FOUND\r\n"));

    f.channel.reset_observations();
    CHECK(accept_inline(f, "not-a-command\r\n"));
    f.session->drive();
    CHECK_EQ(f.channel.inline_stream, std::string("ERROR\r\n"));

    f.channel.reset_observations();
    CHECK(accept_inline(f, "stats\r\n"));
    f.session->drive();
    CHECK(f.channel.bulk_sends.empty());
    CHECK(f.channel.inline_stream.find("STAT version goblin-store 0.0.2\r\n") !=
          std::string::npos);
    CHECK(f.channel.inline_stream.find("STAT cmd_set 1\r\n") != std::string::npos);
    CHECK(f.channel.inline_stream.find("STAT sets_stored 1\r\n") != std::string::npos);
    CHECK(f.channel.inline_stream.find("STAT rdma_bulk_rx 1\r\n") != std::string::npos);
    CHECK(f.channel.inline_stream.ends_with("END\r\n"));
}
