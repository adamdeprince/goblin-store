#include "goblin/store/client.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using goblin::client::Client;
using goblin::client::ConnectionError;
using goblin::client::DeleteResult;
using goblin::client::ExasockOptions;
using goblin::client::ProtocolError;
using goblin::client::ServerError;
using goblin::client::StoreResult;
using goblin::client::TimeoutError;
using goblin::client::Transport;
using goblin::client::ValueTooLargeError;

class FakeTransport final : public Transport {
public:
    struct Emission {
        bool bulk = false;
        std::string bytes;
    };

    explicit FakeTransport(std::vector<std::string> replies, std::size_t max_fragment = 7,
                           std::size_t max_bulk_fragment = 5)
        : inbound_(replies.begin(), replies.end()), maximum_(max_fragment),
          bulk_maximum_(max_bulk_fragment) {}

    std::size_t max_fragment_size() const noexcept override { return maximum_; }
    bool try_send(std::string_view fragment) noexcept override {
        if (closed_ || failed_ || peer_disconnected_) return false;
        if (blocked_sends_ != 0) {
            --blocked_sends_;
            return false;
        }
        if (send_delay_ != std::chrono::microseconds::zero())
            std::this_thread::sleep_for(send_delay_);
        sent_.append(fragment);
        emissions_.push_back(Emission{false, std::string(fragment)});
        send_threads_.push_back(std::this_thread::get_id());
        return true;
    }
    std::size_t max_bulk_fragment_size() const noexcept override { return bulk_maximum_; }
    bool try_send_bulk(std::string_view fragment) noexcept override {
        if (closed_ || failed_ || peer_disconnected_ || fragment.empty() ||
            fragment.size() > bulk_maximum_)
            return false;
        if (blocked_bulk_sends_ != 0) {
            --blocked_bulk_sends_;
            return false;
        }
        if (send_delay_ != std::chrono::microseconds::zero())
            std::this_thread::sleep_for(send_delay_);
        bulk_sent_.append(fragment);
        bulk_fragments_.emplace_back(fragment);
        emissions_.push_back(Emission{true, std::string(fragment)});
        send_threads_.push_back(std::this_thread::get_id());
        return true;
    }
    std::optional<std::string_view> peek() noexcept override {
        if (closed_ || failed_ || peer_disconnected_ || inbound_.empty())
            return std::nullopt;
        if (receive_delay_ != std::chrono::microseconds::zero())
            std::this_thread::sleep_for(receive_delay_);
        return std::string_view(inbound_.front());
    }
    void pop() noexcept override { inbound_.pop_front(); }
    bool failed() const noexcept override { return failed_; }
    bool disconnected() const noexcept override { return closed_ || peer_disconnected_; }
    std::string_view error() const noexcept override { return error_; }
    void wait() noexcept override {
        ++waits_;
        if (fail_on_wait_ && waits_ >= *fail_on_wait_) {
            failed_ = true;
            if (error_.empty()) error_ = "injected transport failure";
        }
        if (disconnect_on_wait_ && waits_ >= *disconnect_on_wait_)
            peer_disconnected_ = true;
    }
    void close() noexcept override { closed_ = true; }

    const std::string& sent() const noexcept { return sent_; }
    const std::string& bulk_sent() const noexcept { return bulk_sent_; }
    const std::vector<std::string>& bulk_fragments() const noexcept { return bulk_fragments_; }
    const std::vector<Emission>& emissions() const noexcept { return emissions_; }
    bool closed() const noexcept { return closed_; }
    std::size_t waits() const noexcept { return waits_; }
    const std::vector<std::thread::id>& send_threads() const noexcept { return send_threads_; }

    void block_sends(std::size_t count) noexcept { blocked_sends_ = count; }
    void block_bulk_sends(std::size_t count) noexcept { blocked_bulk_sends_ = count; }
    void fail_on_wait(std::size_t count, std::string error) {
        fail_on_wait_ = count;
        error_ = std::move(error);
    }
    void disconnect_on_wait(std::size_t count) noexcept { disconnect_on_wait_ = count; }
    void set_send_delay(std::chrono::microseconds delay) noexcept { send_delay_ = delay; }
    void set_receive_delay(std::chrono::microseconds delay) noexcept { receive_delay_ = delay; }

private:
    std::deque<std::string> inbound_;
    std::size_t maximum_;
    std::size_t bulk_maximum_;
    std::string sent_;
    std::string bulk_sent_;
    std::string error_;
    std::vector<std::string> bulk_fragments_;
    std::vector<Emission> emissions_;
    std::vector<std::thread::id> send_threads_;
    std::size_t blocked_sends_ = 0;
    std::size_t blocked_bulk_sends_ = 0;
    std::size_t waits_ = 0;
    std::optional<std::size_t> fail_on_wait_;
    std::optional<std::size_t> disconnect_on_wait_;
    std::chrono::microseconds send_delay_{0};
    std::chrono::microseconds receive_delay_{0};
    bool failed_ = false;
    bool peer_disconnected_ = false;
    bool closed_ = false;
};

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "goblin_store_client_tests: " << message << '\n';
    std::exit(1);
}

void check(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

template <class Exception, class Function>
void check_throws(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
    }
    fail(message);
}

std::vector<std::string> split_at(const std::string& bytes, std::size_t cut) {
    std::vector<std::string> pieces;
    if (cut != 0) pieces.push_back(bytes.substr(0, cut));
    if (cut != bytes.size()) pieces.push_back(bytes.substr(cut));
    return pieces;
}

std::pair<Client, FakeTransport*> client_for(std::vector<std::string> replies,
                                             std::uint64_t max_value = 0,
                                             std::chrono::milliseconds timeout =
                                                 std::chrono::milliseconds{100},
                                             std::size_t max_fragment = 7,
                                             std::size_t max_bulk_fragment = 5) {
    auto fake = std::make_unique<FakeTransport>(std::move(replies), max_fragment,
                                                max_bulk_fragment);
    FakeTransport* raw = fake.get();
    return {Client(std::move(fake), timeout, max_value), raw};
}

void test_get_at_every_boundary() {
    const std::string value("a\0b\r\nx", 6);
    const std::string response = "VALUE blob 17 6\r\n" + value + "\r\nEND\r\n";
    for (std::size_t cut = 0; cut <= response.size(); ++cut) {
        auto [client, transport] = client_for(split_at(response, cut));
        const auto item = client.get("blob");
        check(item.has_value(), "GET hit became a miss");
        check(item->value == value, "GET body changed at a fragment boundary");
        check(item->flags == 17 && item->size == value.size() && !item->cas,
              "GET metadata is wrong");
        check(transport->sent() == "get blob\r\n", "GET request framing is wrong");
    }

    std::vector<std::string> bytewise;
    for (const char ch : response) bytewise.emplace_back(1, ch);
    auto [client, unused] = client_for(std::move(bytewise));
    check(client.get("blob")->value == value, "bytewise fragmented GET failed");
    (void)unused;
}

void test_gets_miss_and_streaming() {
    auto [miss, miss_transport] = client_for({"E", "ND\r\n"});
    check(!miss.get("missing"), "GET miss was not recognized");
    check(miss_transport->sent() == "get missing\r\n", "miss request is wrong");

    auto [gets, gets_transport] = client_for({"VALUE k 9 5 184467\r\nhe", "llo\r\nEND\r\n"});
    const auto item = gets.gets("k");
    check(item && item->value == "hello" && item->flags == 9 && item->cas == 184467,
          "gets metadata/body is wrong");
    check(gets_transport->sent() == "gets k\r\n", "gets request is wrong");

    auto [stream, stream_transport] = client_for({"VALUE big 0 8\r\nabc", "defgh\r\nEND\r\n"});
    std::string collected;
    const auto info = stream.get_to("big", [&](std::string_view bytes) { collected += bytes; });
    check(info && info->size == 8 && collected == "abcdefgh", "streaming GET failed");
    check(stream_transport->sent() == "get big\r\n", "streaming request is wrong");
}

void test_storage_and_delete() {
    const std::string binary("x\0y", 3);
    auto [set, set_transport] = client_for({"ST", "ORED\r\n"});
    check(set.set("key", binary, 42, 60) == StoreResult::stored, "set failed");
    check(set_transport->sent() == "set key 42 60 3\r\n\r\n",
          "binary SET control framing is wrong");
    check(set_transport->bulk_sent() == binary, "binary SET bulk body is wrong");

    auto [add, add_transport] = client_for({"NOT_STORED\r\n"});
    check(add.add("key", "") == StoreResult::not_stored, "add result is wrong");
    check(add_transport->sent() == "add key 0 0 0\r\n\r\n", "empty ADD framing is wrong");
    check(add_transport->bulk_fragments().empty(),
          "empty ADD emitted a meaningless bulk fragment");

    auto [replace, replace_transport] = client_for({"STORED\r\n"});
    check(replace.replace("key", "v") == StoreResult::stored, "replace failed");
    check(replace_transport->sent() == "replace key 0 0 1\r\n\r\n",
          "replace control framing is wrong");
    check(replace_transport->bulk_sent() == "v", "tiny replace did not use bulk transport");

    auto [cas, cas_transport] = client_for({"EXISTS\r\n"});
    check(cas.compare_exchange("key", "new", 99, 4, 3) == StoreResult::exists,
          "cas result is wrong");
    check(cas_transport->sent() == "cas key 4 3 3 99\r\n\r\n",
          "cas control framing is wrong");
    check(cas_transport->bulk_sent() == "new", "cas body did not use bulk transport");

    auto [del, del_transport] = client_for({"DELETED\r\n"});
    check(del.erase("key") == DeleteResult::deleted, "delete failed");
    check(del_transport->sent() == "delete key\r\n", "delete framing is wrong");
}

void test_bulk_storage_boundaries_and_ordering() {
    auto [client, transport] =
        client_for({"STORED\r\n"}, 0, std::chrono::milliseconds{100}, 64, 4);
    check(client.set("key", "abcdefghij") == StoreResult::stored,
          "multi-window SET failed");

    check(transport->sent() == "set key 0 0 10\r\n\r\n",
          "SET body leaked into the inline control stream");
    check(transport->bulk_sent() == "abcdefghij", "bulk SET changed or dropped bytes");
    const std::vector<std::string> expected_chunks{"abcd", "efgh", "ij"};
    check(transport->bulk_fragments() == expected_chunks,
          "SET did not respect negotiated bulk-window boundaries");

    const auto& emissions = transport->emissions();
    check(emissions.size() == 5, "unexpected SET fragment count");
    check(!emissions[0].bulk && emissions[0].bytes == "set key 0 0 10\r\n",
          "SET header was not the first inline fragment");
    check(emissions[1].bulk && emissions[2].bulk && emissions[3].bulk,
          "SET body did not remain entirely on the bulk path");
    check(!emissions[4].bulk && emissions[4].bytes == "\r\n",
          "SET trailer did not follow the bulk body inline");
}

void test_bulk_storage_backpressure_and_failures() {
    auto [backpressured, transport] =
        client_for({"STORED\r\n"}, 0, std::chrono::milliseconds{100}, 64, 3);
    transport->block_bulk_sends(4);
    check(backpressured.set("key", "abcdef") == StoreResult::stored,
          "bulk backpressure changed the storage result");
    check(transport->waits() == 4,
          "bulk backpressure did not make exactly four progress polls");
    check(transport->bulk_fragments() == std::vector<std::string>{"abc", "def"},
          "bulk backpressure duplicated or dropped a fragment");
    check(transport->sent() == "set key 0 0 6\r\n\r\n",
          "bulk backpressure contaminated the inline stream");

    auto [failed, failed_transport] =
        client_for({}, 0, std::chrono::milliseconds{100}, 64, 3);
    failed_transport->block_bulk_sends(1);
    failed_transport->fail_on_wait(1, "injected bulk-window failure");
    bool saw_diagnostic = false;
    try {
        (void)failed.set("key", "value");
    } catch (const ConnectionError& error) {
        saw_diagnostic = std::string_view(error.what()).find("injected bulk-window failure") !=
                         std::string_view::npos;
    }
    check(saw_diagnostic, "bulk transport failure lost its diagnostic");
    check(failed_transport->closed(), "bulk transport failure did not poison the connection");
    check(failed_transport->sent() == "set key 0 0 5\r\n",
          "bulk failure sent value bytes through the control path");
    check(failed_transport->bulk_sent().empty(),
          "failed first bulk window was incorrectly recorded as sent");
}

void test_admin_and_failures() {
    auto [version, version_transport] = client_for({"VERSION goblin-store 0.0.2\r\n"});
    check(version.version() == "goblin-store 0.0.2", "version parse failed");
    check(version_transport->sent() == "version\r\n", "version request is wrong");

    auto [stats, stats_transport] = client_for({"STAT hits 7\r\nSTAT bytes 99\r\nEND\r\n"});
    const auto values = stats.stats();
    check(values.at("hits") == "7" && values.at("bytes") == "99", "stats parse failed");
    check(stats_transport->sent() == "stats\r\n", "stats request is wrong");

    auto [bad, bad_transport] = client_for({"VALUE wrong 0 1\r\nx\r\nEND\r\n"});
    check_throws<ProtocolError>([&] { (void)bad.get("right"); },
                                "mismatched response key was accepted");
    check(bad_transport->closed(), "protocol failure did not poison the connection");

    auto [large, large_transport] = client_for({"VALUE k 0 9\r\n123456789\r\nEND\r\n"}, 8);
    check_throws<ValueTooLargeError>([&] { (void)large.get("k"); },
                                     "materialization limit was ignored");
    check(large_transport->closed(), "oversize response did not close the connection");

    auto [timeout, timeout_transport] = client_for({}, 0, std::chrono::milliseconds{0});
    check_throws<TimeoutError>([&] { (void)timeout.get("k"); }, "empty transport did not time out");
    check(timeout_transport->closed(), "timeout did not close the connection");

    auto [invalid, invalid_transport] = client_for({"END\r\n"});
    check_throws<std::invalid_argument>([&] { (void)invalid.get("bad key"); },
                                        "invalid key was accepted");
    check(invalid_transport->sent().empty(), "invalid key reached the wire");
}

void test_backpressure_and_transport_failures() {
    auto [backpressured, backpressure_transport] = client_for({"VERSION ready\r\n"});
    backpressure_transport->block_sends(4);
    check(backpressured.version() == "ready", "send backpressure changed the response");
    check(backpressure_transport->waits() == 4,
          "send backpressure did not make exactly four progress polls");
    check(backpressure_transport->sent() == "version\r\n",
          "send backpressure duplicated or dropped request bytes");

    auto [failed, failed_transport] = client_for({});
    failed_transport->fail_on_wait(1, "injected CQ failure");
    bool saw_failure = false;
    try {
        (void)failed.get("key");
    } catch (const ConnectionError& error) {
        saw_failure = std::string_view(error.what()).find("injected CQ failure") !=
                      std::string_view::npos;
    }
    check(saw_failure, "transport failure did not retain its diagnostic");
    check(failed_transport->closed(), "transport failure did not poison the connection");

    auto [disconnected, disconnected_transport] = client_for({});
    disconnected_transport->disconnect_on_wait(1);
    check_throws<ConnectionError>([&] { (void)disconnected.get("key"); },
                                  "peer disconnect was not reported as a connection error");
    check(disconnected_transport->closed(), "peer disconnect did not close the client transport");
}

void test_continuous_progress_deadlines() {
    using namespace std::chrono_literals;

    auto [sending, sending_transport] =
        client_for({"STORED\r\n"}, 0, std::chrono::milliseconds{40}, 1);
    sending_transport->set_send_delay(60ms);
    check_throws<TimeoutError>([&] { (void)sending.set("key", "a value"); },
                               "continuously progressing sends escaped the operation deadline");
    check(!sending_transport->sent().empty(),
          "continuous-send deadline test made no successful wire progress");
    check(sending_transport->closed(), "continuous-send timeout did not poison the connection");

    auto [receiving, receiving_transport] =
        client_for({"V", "ERSION ready\r\n"}, 0, std::chrono::milliseconds{40});
    receiving_transport->set_receive_delay(60ms);
    check_throws<TimeoutError>([&] { (void)receiving.version(); },
                               "continuously progressing receives escaped the operation deadline");
    check(receiving_transport->closed(),
          "continuous-receive timeout did not poison the connection");
}

void test_server_error_reuse_and_sink_failure() {
    auto [server_error, server_error_transport] =
        client_for({"SERVER_ERROR temporarily unavailable\r\n", "END\r\n"});
    check_throws<ServerError>([&] { (void)server_error.get("key"); },
                              "SERVER_ERROR was not surfaced to the caller");
    check(!server_error_transport->closed(), "SERVER_ERROR unnecessarily poisoned the connection");
    check(!server_error.get("key"), "connection could not be reused after SERVER_ERROR");
    check(server_error_transport->sent() == "get key\r\nget key\r\n",
          "reuse after SERVER_ERROR changed request framing");

    struct SinkFailure {};
    auto [sink_error, sink_error_transport] =
        client_for({"VALUE key 0 4\r\ndata\r\nEND\r\n"});
    check_throws<SinkFailure>(
        [&] {
            (void)sink_error.get_to("key", [](std::string_view) { throw SinkFailure{}; });
        },
        "streaming sink exception was not propagated");
    check(sink_error_transport->closed(), "sink exception did not poison the connection");
    check_throws<ConnectionError>([&] { (void)sink_error.get("key"); },
                                  "client reused a connection after a sink exception");
}

void test_reentrancy_rejection_and_recovery() {
    auto [client, transport] =
        client_for({"VALUE key 0 1\r\nx\r\nEND\r\n", "VERSION recovered\r\n"});
    bool rejected = false;
    std::string value;
    const auto info = client.get_to("key", [&](std::string_view bytes) {
        value.append(bytes);
        try {
            (void)client.version();
        } catch (const std::logic_error&) {
            rejected = true;
        }
    });
    check(info && value == "x", "outer transaction did not survive rejected reentrancy");
    check(rejected, "same-client callback reentrancy was not rejected");
    check(!transport->closed(), "caught reentrancy rejection poisoned the connection");
    check(client.version() == "recovered", "transaction guard did not recover after reentrancy");
    check(transport->sent() == "get key\r\nversion\r\n",
          "reentrant operation reached the wire or recovery request was lost");
}

void test_maximum_line_fragmentation() {
    constexpr std::size_t maximum = 8192;
    std::string line = "VERSION ";
    line.append(maximum - line.size(), 'x');
    const std::string expected(maximum - std::string_view("VERSION ").size(), 'x');

    for (std::vector<std::string> fragments :
         {std::vector<std::string>{line, "\r", "\n"},
          std::vector<std::string>{line + "\r", "\n"}}) {
        auto [client, transport] = client_for(std::move(fragments));
        check(client.version() == expected,
              "8192-byte response line depended on the CRLF fragment boundary");
        check(!transport->closed(), "valid maximum-size response line poisoned the connection");
    }

    std::string oversized = line;
    oversized.push_back('x');
    auto [too_long, too_long_transport] = client_for({oversized + "\r\n"});
    check_throws<ProtocolError>([&] { (void)too_long.version(); },
                                "8193-byte response line exceeded the parser limit");
    check(too_long_transport->closed(), "oversized response line did not poison the connection");
}

void test_concurrent_calls_are_serialized() {
    using namespace std::chrono_literals;
    constexpr std::size_t threads = 6;
    std::vector<std::string> replies(threads, "VERSION concurrent\r\n");
    auto [client, transport] = client_for(std::move(replies), 0, 2s, 1);
    transport->set_send_delay(250us);

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::string> results(threads);
    std::vector<std::string> errors(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                results[i] = client.version();
            } catch (const std::exception& error) {
                errors[i] = error.what();
            } catch (...) {
                errors[i] = "non-standard exception";
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    for (std::size_t i = 0; i < threads; ++i) {
        check(errors[i].empty(), "concurrent client call threw an exception");
        check(results[i] == "concurrent", "concurrent client call received the wrong response");
    }
    std::string expected_wire;
    for (std::size_t i = 0; i < threads; ++i) expected_wire += "version\r\n";
    check(transport->sent() == expected_wire, "concurrent requests interleaved on the byte stream");

    const auto& owners = transport->send_threads();
    check(owners.size() == expected_wire.size(), "concurrent request fragment accounting is wrong");
    std::size_t owner_runs = 0;
    std::thread::id previous;
    for (const auto owner : owners) {
        if (owner_runs == 0 || owner != previous) {
            ++owner_runs;
            previous = owner;
        }
    }
    check(owner_runs == threads, "one client's concurrent calls were not transaction-serialized");
}

void test_exasock_build_boundary() {
    if (goblin::client::exasock_available()) return;
    check(!goblin::client::exasock_active(),
          "disabled ExaSock backend reported an active preload library");
    ExasockOptions options;
    options.address = "192.0.2.1";
    options.connect_timeout = std::chrono::milliseconds{0};
    check_throws<ConnectionError>(
        [&] { (void)Client::connect_exasock(options); },
        "disabled ExaSock backend did not fail explicitly");
}

} // namespace

int main() {
    test_get_at_every_boundary();
    test_gets_miss_and_streaming();
    test_storage_and_delete();
    test_bulk_storage_boundaries_and_ordering();
    test_bulk_storage_backpressure_and_failures();
    test_admin_and_failures();
    test_backpressure_and_transport_failures();
    test_continuous_progress_deadlines();
    test_server_error_reuse_and_sink_failure();
    test_reentrancy_rejection_and_recovery();
    test_maximum_line_fragmentation();
    test_concurrent_calls_are_serialized();
    test_exasock_build_boundary();
    std::cout << "goblin_store_client_tests: all tests passed\n";
}
