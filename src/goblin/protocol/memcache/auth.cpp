#include "goblin/protocol/memcache/auth.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace goblin::memcache {
namespace {

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned ac = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned bc = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        difference |= ac ^ bc;
    }
    return difference == 0;
}

} // namespace

Result<Authenticator> Authenticator::load(std::string_view path) {
    const std::string owned(path);
    const int descriptor = ::open(owned.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return err(Errc::io_error,
                   std::string("open auth file: ") + std::strerror(errno));
    struct stat info{};
    if (::fstat(descriptor, &info) < 0) {
        const int saved = errno;
        ::close(descriptor);
        return err(Errc::io_error, std::string("read auth file: ") + std::strerror(saved));
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(descriptor);
        return err(Errc::invalid_argument, "auth file must be a regular file");
    }
    if ((info.st_mode & 0077) != 0) {
        ::close(descriptor);
        return err(Errc::invalid_argument,
                   "auth file must not be accessible by group or other users (chmod 600)");
    }

    FILE* input = ::fdopen(descriptor, "r");
    if (!input) {
        const int saved = errno;
        ::close(descriptor);
        return err(Errc::io_error, std::string("open auth stream: ") + std::strerror(saved));
    }
    Authenticator result;
    char* raw_line = nullptr;
    std::size_t capacity = 0;
    std::size_t line_number = 0;
    for (;;) {
        errno = 0;
        const ssize_t length = ::getline(&raw_line, &capacity, input);
        if (length < 0) break;
        ++line_number;
        std::string line(raw_line, static_cast<std::size_t>(length));
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 256) {
            std::free(raw_line);
            ::fclose(input);
            return err(Errc::invalid_argument,
                       "auth file entry exceeds 256 bytes at line " +
                           std::to_string(line_number));
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == line.size()) {
            std::free(raw_line);
            ::fclose(input);
            return err(Errc::invalid_argument,
                       "auth file needs user:password at line " +
                           std::to_string(line_number));
        }
        result.entries_.emplace_back(line.substr(0, colon), line.substr(colon + 1));
    }
    const int read_error = errno;
    std::free(raw_line);
    const bool stream_error = ::ferror(input) != 0;
    ::fclose(input); // also closes descriptor
    if (stream_error)
        return err(Errc::io_error,
                   std::string("read auth file: ") + std::strerror(read_error));
    if (result.entries_.empty())
        return err(Errc::invalid_argument, "auth file contains no credentials");
    return result;
}

bool Authenticator::authenticate(std::string_view token) const noexcept {
    const std::size_t separator = token.find(' ');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == token.size())
        return false;
    const std::string_view user = token.substr(0, separator);
    const std::string_view password = token.substr(separator + 1);
    bool matched = false;
    for (const auto& [expected_user, expected_password] : entries_) {
        const bool user_match = constant_time_equal(user, expected_user);
        const bool password_match = constant_time_equal(password, expected_password);
        matched |= user_match & password_match; // never skip the password comparison
    }
    return matched;
}

} // namespace goblin::memcache
