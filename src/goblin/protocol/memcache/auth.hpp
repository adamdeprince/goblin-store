#pragma once

#include "goblin/common/error.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::memcache {

// Memcached ASCII authentication file: one `username:password` pair per line.
class Authenticator {
public:
    static Result<Authenticator> load(std::string_view path);
    bool authenticate(std::string_view token) const noexcept; // token is `username password`
    std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

} // namespace goblin::memcache
