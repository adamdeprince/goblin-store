#include <goblin/store/client.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace {

class NullTransport final : public goblin::client::Transport {
public:
    std::size_t max_fragment_size() const noexcept override { return 1; }
    bool try_send(std::string_view) noexcept override { return false; }
    std::size_t max_bulk_fragment_size() const noexcept override { return 1; }
    bool try_send_bulk(std::string_view) noexcept override { return false; }
    std::optional<std::string_view> peek() noexcept override { return std::nullopt; }
    void pop() noexcept override {}
    bool failed() const noexcept override { return false; }
    bool disconnected() const noexcept override { return true; }
    std::string_view error() const noexcept override { return {}; }
    void wait() noexcept override {}
    void close() noexcept override {}
};

} // namespace

int main() {
    goblin::client::Options options;
    options.address = "127.0.0.1";
    goblin::client::Client client(std::make_unique<NullTransport>());
    client.close();
    return options.port == 11211 ? 0 : 1;
}
