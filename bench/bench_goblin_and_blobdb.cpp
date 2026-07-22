// Sequential object-cache comparison: embedded Goblin Store versus integrated BlobDB.
//
// The harness owns both databases' complete lifecycle. For each selected I/O mode it creates an
// empty database, loads every key from --prefetch in file order, then replays --schedule one key
// at a time. Timed retrievals never overlap. Per-request first-byte and
// completion results stay in RAM until an engine's read phase ends, so writing the CSV cannot
// perturb measured latency.

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/version.h>
#include <rocksdb/write_buffer_manager.h>

#include "goblin/store.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;
constexpr std::uint64_t GiB = 1024 * MiB;
constexpr std::size_t kCopyBuffer = 1 * MiB;

#if defined(CLOCK_MONOTONIC_RAW)
constexpr clockid_t kBenchmarkClock = CLOCK_MONOTONIC_RAW;
constexpr std::string_view kBenchmarkClockName = "CLOCK_MONOTONIC_RAW";
#else
constexpr clockid_t kBenchmarkClock = CLOCK_MONOTONIC;
constexpr std::string_view kBenchmarkClockName = "CLOCK_MONOTONIC";
#endif

enum class IoMode { buffered, direct };
enum class SelectedCase { all, goblin, blobdb_buffered, blobdb_direct };

std::string_view mode_name(IoMode mode) {
    return mode == IoMode::direct ? "direct" : "buffered";
}

std::string_view selected_case_name(SelectedCase selected) {
    switch (selected) {
        case SelectedCase::all: return "all";
        case SelectedCase::goblin: return "goblin";
        case SelectedCase::blobdb_buffered: return "blobdb-buffered";
        case SelectedCase::blobdb_direct: return "blobdb-direct";
    }
    return "unknown";
}

std::string rocksdb_version() {
    return std::to_string(ROCKSDB_MAJOR) + '.' + std::to_string(ROCKSDB_MINOR) + '.' +
           std::to_string(ROCKSDB_PATCH);
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

struct Args {
    fs::path data;
    fs::path scratch;
    fs::path prefetch;
    fs::path schedule;
    fs::path output;
    // Nearest convenient whole-2-MiB settings to 1.6 GiB + 4.2 GiB.
    std::uint64_t goblin_small_memory = 1640 * MiB;
    std::uint64_t goblin_large_memory = 4300 * MiB;
    std::uint64_t goblin_head_size = 16 * KiB;
    std::uint64_t goblin_read_chunk = 4 * MiB;
    std::uint64_t goblin_file_handles = 256 * KiB;
    std::uint64_t blobdb_buffer = 5940 * MiB;
    std::uint64_t blobdb_min_blob_size = 16 * KiB;
    int blobdb_cache_shard_bits = 4;
    SelectedCase selected_case = SelectedCase::all;
    bool keep_databases = false;
    bool drop_source_cache = true;
};

struct Object {
    std::string key;
    fs::path path;
    std::uint64_t bytes = 0;
};

struct ScheduledKey {
    std::size_t object_index = 0;
};

enum class ReadStatus { ok, missing, error };

std::string_view status_name(ReadStatus status) {
    switch (status) {
        case ReadStatus::ok: return "ok";
        case ReadStatus::missing: return "missing";
        case ReadStatus::error: return "error";
    }
    return "error";
}

struct ReadRecord {
    std::uint64_t time_to_first_byte_ns = 0;
    std::uint64_t time_to_end_ns = 0;
    std::uint64_t returned_bytes = 0;
    ReadStatus status = ReadStatus::error;
};

struct RunSummary {
    std::string engine;
    IoMode mode = IoMode::buffered;
    std::uint64_t population_ns = 0;
    std::uint64_t populated_objects = 0;
    std::uint64_t populated_bytes = 0;
    std::uint64_t read_ns = 0;
    std::uint64_t read_requests = 0;
    std::uint64_t read_bytes = 0;
};

std::uint64_t now_ns() {
    timespec value{};
    if (::clock_gettime(kBenchmarkClock, &value) != 0)
        throw std::runtime_error(std::string("clock_gettime: ") + std::strerror(errno));
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

[[noreturn]] void usage(const char* argv0, std::string_view error = {}) {
    if (!error.empty()) std::cerr << "error: " << error << "\n\n";
    std::cerr
        << "usage: " << argv0
        << " --data DIR --scratch DIR --prefetch FILE --schedule FILE [options]\n"
        << "\n"
        << "Required inputs:\n"
        << "  --data DIR                    source tree to load (--source alias)\n"
        << "  --scratch DIR                 parent for disposable engine databases\n"
        << "  --prefetch FILE              keys to preload, one per line\n"
        << "  --schedule FILE               one relative key per line (--keys alias)\n"
        << "\n"
        << "Memory and execution:\n"
        << "  --goblin-small-memory SIZE    dedicated packed-object pool [1640M ~= 1.6G]\n"
        << "  --goblin-large-memory SIZE    fixed-head pool [4300M ~= 4.2G]\n"
        << "  --goblin-head-size SIZE       per-object resident head [16K]\n"
        << "  --goblin-read-chunk SIZE      aligned tail-read quantum [4M]\n"
        << "  --goblin-file-handles N       read descriptor cache, power of two [262144]\n"
        << "  --blobdb-buffer SIZE          shared block/blob-cache/memtable budget [5940M]\n"
        << "  --blobdb-min-blob-size SIZE   values at or above SIZE use blob files [16K]\n"
        << "  --blobdb-cache-shard-bits N   BlobDB cache uses 2^N shards [4]\n"
        << "  --case CASE                   all|goblin|blobdb-buffered|blobdb-direct [all]\n"
        << "\n"
        << "Artifacts and cleanup:\n"
        << "  --output DIR                  result directory [timestamped in cwd]\n"
        << "  --keep-databases              retain disposable database directories\n"
        << "  --keep-source-cache           do not POSIX_FADV_DONTNEED source files\n"
        << "  --help                        show this text\n"
        << "\n"
        << "SIZE accepts a decimal number and K, M, G, KiB, MiB, or GiB suffix. Goblin\n"
        << "pool sizes must resolve to a multiple of its 2 MiB x86 allocation block.\n";
    std::exit(error.empty() ? 0 : 2);
}

std::uint64_t parse_u64(std::string_view input, std::string_view option) {
    std::uint64_t value = 0;
    const auto [end, ec] =
        std::from_chars(input.data(), input.data() + input.size(), value);
    if (input.empty() || ec != std::errc{} || end != input.data() + input.size())
        throw std::runtime_error("invalid " + std::string(option) + ": " + std::string(input));
    return value;
}

std::uint64_t parse_size(std::string_view input, std::string_view option) {
    std::string lower(input);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }

    std::uint64_t multiplier = 1;
    auto remove_suffix = [&](std::string_view suffix, std::uint64_t value) {
        if (ends_with(lower, suffix)) {
            lower.resize(lower.size() - suffix.size());
            multiplier = value;
            return true;
        }
        return false;
    };
    if (!remove_suffix("gib", GiB) && !remove_suffix("mib", MiB) &&
        !remove_suffix("kib", KiB) && !remove_suffix("gb", GiB) &&
        !remove_suffix("mb", MiB) && !remove_suffix("kb", KiB) &&
        !remove_suffix("g", GiB) && !remove_suffix("m", MiB))
        (void)remove_suffix("k", KiB);

    double number = 0.0;
    const auto [end, ec] =
        std::from_chars(lower.data(), lower.data() + lower.size(), number,
                        std::chars_format::general);
    if (lower.empty() || ec != std::errc{} || end != lower.data() + lower.size() ||
        !std::isfinite(number) || number <= 0.0)
        throw std::runtime_error("invalid " + std::string(option) + ": " + std::string(input));
    const long double bytes = static_cast<long double>(number) * multiplier;
    if (bytes > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        throw std::runtime_error(std::string(option) + " is too large");
    return static_cast<std::uint64_t>(std::floor(bytes + 0.5L));
}

std::string timestamp_name() {
    const std::time_t current = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_r(&current, &utc) == nullptr) return "goblin-blobdb-results";
    std::array<char, 64> buffer{};
    if (std::strftime(buffer.data(), buffer.size(),
                      "goblin-blobdb-%Y%m%dT%H%M%SZ", &utc) == 0)
        return "goblin-blobdb-results";
    return buffer.data();
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        auto next = [&]() -> std::string_view {
            if (++index >= argc) usage(argv[0], std::string(option) + " needs an argument");
            return argv[index];
        };
        if (option == "--data" || option == "--source") args.data = next();
        else if (option == "--scratch") args.scratch = next();
        else if (option == "--prefetch") args.prefetch = next();
        else if (option == "--schedule" || option == "--keys") args.schedule = next();
        else if (option == "--output") args.output = next();
        else if (option == "--goblin-small-memory")
            args.goblin_small_memory = parse_size(next(), option);
        else if (option == "--goblin-large-memory")
            args.goblin_large_memory = parse_size(next(), option);
        else if (option == "--goblin-head-size")
            args.goblin_head_size = parse_size(next(), option);
        else if (option == "--goblin-read-chunk")
            args.goblin_read_chunk = parse_size(next(), option);
        else if (option == "--goblin-file-handles")
            args.goblin_file_handles = parse_u64(next(), option);
        else if (option == "--blobdb-buffer" || option == "--rocksdb-buffer")
            args.blobdb_buffer = parse_size(next(), option);
        else if (option == "--blobdb-min-blob-size")
            args.blobdb_min_blob_size = parse_size(next(), option);
        else if (option == "--blobdb-cache-shard-bits") {
            const auto value = parse_u64(next(), option);
            if (value > 19) usage(argv[0], "--blobdb-cache-shard-bits must be between 0 and 19");
            args.blobdb_cache_shard_bits = static_cast<int>(value);
        } else if (option == "--case") {
            const auto value = next();
            if (value == "all") args.selected_case = SelectedCase::all;
            else if (value == "goblin") args.selected_case = SelectedCase::goblin;
            else if (value == "blobdb-buffered" || value == "rocksdb-buffered")
                args.selected_case = SelectedCase::blobdb_buffered;
            else if (value == "blobdb-direct" || value == "rocksdb-direct")
                args.selected_case = SelectedCase::blobdb_direct;
            else
                usage(argv[0],
                      "--case must be all, goblin, blobdb-buffered, or blobdb-direct");
        } else if (option == "--keep-databases") args.keep_databases = true;
        else if (option == "--keep-source-cache") args.drop_source_cache = false;
        else if (option == "--help" || option == "-h") usage(argv[0]);
        else usage(argv[0], "unknown option " + std::string(option));
    }

    if (args.data.empty()) usage(argv[0], "--data is required");
    if (args.scratch.empty()) usage(argv[0], "--scratch is required");
    if (args.prefetch.empty()) usage(argv[0], "--prefetch is required");
    if (args.schedule.empty()) usage(argv[0], "--schedule is required");
    if (args.output.empty()) args.output = fs::current_path() / timestamp_name();
    constexpr std::uint64_t block = 2 * MiB;
    if (args.goblin_small_memory % block != 0 || args.goblin_large_memory % block != 0)
        usage(argv[0], "Goblin memory pools must be multiples of 2 MiB on x86");
    if ((args.goblin_head_size & (args.goblin_head_size - 1)) != 0 ||
        args.goblin_head_size > block)
        usage(argv[0], "--goblin-head-size must be a power of two no larger than 2 MiB");
    if (args.goblin_read_chunk < 4 * KiB ||
        (args.goblin_read_chunk & (args.goblin_read_chunk - 1)) != 0)
        usage(argv[0], "--goblin-read-chunk must be a power of two at least 4 KiB");
    if (args.goblin_file_handles == 0 ||
        (args.goblin_file_handles & (args.goblin_file_handles - 1)) != 0 ||
        args.goblin_file_handles > std::numeric_limits<unsigned>::max())
        usage(argv[0], "--goblin-file-handles must be a nonzero power of two fitting unsigned");
    return args;
}

void raise_file_descriptor_limit(std::uint64_t cache_capacity) {
    constexpr rlim_t overhead = 4096;
    if (cache_capacity > static_cast<std::uint64_t>(RLIM_INFINITY - overhead))
        throw std::runtime_error("Goblin file-handle cache is too large for RLIMIT_NOFILE");
    const rlim_t needed = static_cast<rlim_t>(cache_capacity) + overhead;
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        throw std::runtime_error(std::string("getrlimit(RLIMIT_NOFILE): ") +
                                 std::strerror(errno));
    if (limit.rlim_cur >= needed || limit.rlim_cur == RLIM_INFINITY) return;
    if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < needed)
        throw std::runtime_error(
            "RLIMIT_NOFILE hard limit is too low for --goblin-file-handles; run `ulimit -n " +
            std::to_string(needed) + "` before the benchmark");
    limit.rlim_cur = needed;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
        throw std::runtime_error(
            std::string("setrlimit(RLIMIT_NOFILE) failed: ") + std::strerror(errno) +
            "; run `ulimit -n " + std::to_string(needed) + "` before the benchmark");
}

bool path_prefix(const fs::path& parent, const fs::path& child) {
    auto left = parent.begin();
    auto right = child.begin();
    for (; left != parent.end() && right != child.end(); ++left, ++right) {
        if (*left != *right) return false;
    }
    return left == parent.end();
}

fs::path canonical_existing(const fs::path& path, std::string_view description) {
    std::error_code error;
    const fs::path result = fs::canonical(path, error);
    if (error)
        throw std::runtime_error("cannot resolve " + std::string(description) + " " +
                                 path.string() + ": " + error.message());
    return result;
}

std::string normalize_key(std::string key, std::size_t line = 0,
                          std::string_view list_name = "key list") {
    if (!key.empty() && key.back() == '\r') key.pop_back();
    while (!key.empty() && key.front() == '/') key.erase(key.begin());
    const std::string where = line == 0 ? std::string{} :
        " on " + std::string(list_name) + " line " + std::to_string(line);
    if (key.empty()) throw std::runtime_error("empty key" + where);
    if (key.size() > 250) throw std::runtime_error("key exceeds 250 bytes" + where + ": " + key);
    for (const unsigned char ch : key) {
        if (ch <= 0x20 || ch == 0x7f)
            throw std::runtime_error("key contains whitespace/control bytes" + where + ": " + key);
    }
    const fs::path as_path(key);
    if (as_path.is_absolute()) throw std::runtime_error("absolute key is not allowed" + where);
    for (const auto& component : as_path) {
        if (component == "..") throw std::runtime_error("key escapes --data" + where + ": " + key);
    }
    return as_path.lexically_normal().generic_string();
}

std::vector<Object> inventory(const fs::path& root) {
    std::vector<Object> objects;
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto entry = *iterator;
        iterator.increment(error);
        std::error_code item_error;
        if (!entry.is_regular_file(item_error) || item_error) continue;
        const auto relative = fs::relative(entry.path(), root, item_error);
        if (item_error) throw std::runtime_error("cannot make source path relative");
        const auto size = entry.file_size(item_error);
        if (item_error)
            throw std::runtime_error("cannot stat " + entry.path().string() + ": " +
                                     item_error.message());
        objects.push_back({normalize_key(relative.generic_string()), entry.path(), size});
    }
    if (error) throw std::runtime_error("cannot walk --data: " + error.message());
    std::sort(objects.begin(), objects.end(), [](const Object& left, const Object& right) {
        return left.key < right.key;
    });
    for (std::size_t index = 1; index < objects.size(); ++index) {
        if (objects[index - 1].key == objects[index].key)
            throw std::runtime_error("duplicate normalized source key: " + objects[index].key);
    }
    if (objects.empty()) throw std::runtime_error("--data contains no regular files");
    return objects;
}

std::vector<ScheduledKey> load_key_list(const fs::path& path,
                                        const std::vector<Object>& objects,
                                        std::string_view list_name,
                                        bool reject_duplicates) {
    std::unordered_map<std::string, std::size_t> indexes;
    indexes.reserve(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index)
        indexes.emplace(objects[index].key, index);

    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open schedule " + path.string());
    std::vector<ScheduledKey> result;
    std::vector<bool> selected;
    if (reject_duplicates) selected.resize(objects.size());
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        std::string key = normalize_key(std::move(line), line_number, list_name);
        const auto found = indexes.find(key);
        if (found == indexes.end())
            throw std::runtime_error(std::string(list_name) +
                                     " key is absent from --data on line " +
                                     std::to_string(line_number) + ": " + key);
        if (reject_duplicates && selected[found->second])
            throw std::runtime_error("duplicate " + std::string(list_name) +
                                     " key on line " + std::to_string(line_number) + ": " + key);
        if (reject_duplicates) selected[found->second] = true;
        result.push_back({found->second});
    }
    if (result.empty())
        throw std::runtime_error("--" + std::string(list_name) + " has no keys");
    return result;
}

void validate_schedule_is_prefetched(const std::vector<ScheduledKey>& prefetch,
                                     const std::vector<ScheduledKey>& schedule,
                                     const std::vector<Object>& objects) {
    std::vector<bool> loaded(objects.size());
    for (const auto& entry : prefetch) loaded[entry.object_index] = true;
    for (const auto& entry : schedule) {
        if (!loaded[entry.object_index])
            throw std::runtime_error("scheduled key was not preloaded: " +
                                     objects[entry.object_index].key);
    }
}

std::uint64_t sum_bytes(const std::vector<Object>& objects) {
    std::uint64_t result = 0;
    for (const auto& object : objects) {
        if (object.bytes > std::numeric_limits<std::uint64_t>::max() - result)
            throw std::runtime_error("source byte count overflows uint64_t");
        result += object.bytes;
    }
    return result;
}

std::uint64_t sum_selected_bytes(const std::vector<Object>& objects,
                                 const std::vector<ScheduledKey>& selected) {
    std::uint64_t result = 0;
    for (const auto& entry : selected) {
        const std::uint64_t bytes = objects[entry.object_index].bytes;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - result)
            throw std::runtime_error("selected source byte count overflows uint64_t");
        result += bytes;
    }
    return result;
}

struct GoblinResidentFootprint {
    std::uint64_t small_bytes = 0;
    std::uint64_t fixed_head_bytes = 0;
    std::uint64_t small_objects = 0;
    std::uint64_t fixed_head_objects = 0;
};

GoblinResidentFootprint goblin_resident_footprint(
    const std::vector<Object>& objects, const std::vector<ScheduledKey>& selected,
    std::uint64_t head_size) {
    GoblinResidentFootprint result;
    for (const auto& entry : selected) {
        const std::uint64_t bytes = objects[entry.object_index].bytes;
        if (bytes == 0) continue;
        if (bytes < head_size) {
            const std::uint64_t aligned = (bytes + 7) & ~std::uint64_t{7};
            if (aligned > std::numeric_limits<std::uint64_t>::max() - result.small_bytes)
                throw std::runtime_error("Goblin small-pool footprint overflows uint64_t");
            result.small_bytes += aligned;
            ++result.small_objects;
        } else {
            if (head_size >
                std::numeric_limits<std::uint64_t>::max() - result.fixed_head_bytes)
                throw std::runtime_error("Goblin fixed-head footprint overflows uint64_t");
            result.fixed_head_bytes += head_size;
            ++result.fixed_head_objects;
        }
    }
    return result;
}

void validate_goblin_resident_capacity(const Args& args,
                                       const GoblinResidentFootprint& footprint) {
    if (footprint.small_bytes > args.goblin_small_memory)
        throw std::runtime_error("prefetched RAM-only objects require " +
                                 std::to_string(footprint.small_bytes) +
                                 " small-pool bytes, exceeding --goblin-small-memory " +
                                 std::to_string(args.goblin_small_memory));
    if (footprint.fixed_head_bytes > args.goblin_large_memory)
        throw std::runtime_error("prefetched fixed heads require " +
                                 std::to_string(footprint.fixed_head_bytes) +
                                 " bytes, exceeding --goblin-large-memory " +
                                 std::to_string(args.goblin_large_memory));
}

std::uint64_t goblin_disk_object_count(const std::vector<Object>& objects,
                                       const std::vector<ScheduledKey>& selected,
                                       std::uint64_t head_size) {
    return static_cast<std::uint64_t>(std::count_if(
        selected.begin(), selected.end(), [&](const ScheduledKey& entry) {
            return objects[entry.object_index].bytes > head_size;
        }));
}

std::uint64_t largest_selected_object(const std::vector<Object>& objects,
                                      const std::vector<ScheduledKey>& selected) {
    std::uint64_t largest = 0;
    for (const auto& entry : selected)
        largest = std::max(largest, objects[entry.object_index].bytes);
    return largest;
}

void validate_blobdb_cache_geometry(const Args& args, const std::vector<Object>& objects,
                                    const std::vector<ScheduledKey>& prefetch) {
    const std::uint64_t shards = std::uint64_t{1} << args.blobdb_cache_shard_bits;
    const std::uint64_t bytes_per_shard = args.blobdb_buffer / shards;
    const std::uint64_t largest = largest_selected_object(objects, prefetch);
    if (largest > bytes_per_shard)
        throw std::runtime_error(
            "largest prefetched value is " + std::to_string(largest) +
            " bytes but each strict BlobDB cache shard has only " +
            std::to_string(bytes_per_shard) +
            " bytes; lower --blobdb-cache-shard-bits or increase --blobdb-buffer");
}

void ensure_directory(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) throw std::runtime_error("cannot create " + path.string() + ": " + error.message());
}

void remove_database(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
    if (error) throw std::runtime_error("cannot remove " + path.string() + ": " + error.message());
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    int get() const noexcept { return value_; }

private:
    int value_;
};

FileDescriptor open_source(const Object& object) {
    const int fd = ::open(object.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open " + object.path.string() + ": " +
                                 std::strerror(errno));
    return FileDescriptor(fd);
}

void discard_source_cache(int fd, std::uint64_t size, bool enabled) {
#if defined(POSIX_FADV_DONTNEED)
    if (enabled) (void)::posix_fadvise(fd, 0, static_cast<off_t>(size), POSIX_FADV_DONTNEED);
#else
    (void)fd;
    (void)size;
    (void)enabled;
#endif
}

void discard_selected_source_cache(const std::vector<Object>& objects,
                                   const std::vector<ScheduledKey>& selected,
                                   bool enabled) {
    if (!enabled) return;
    for (const auto& entry : selected) {
        const auto& object = objects[entry.object_index];
        auto fd = open_source(object);
        discard_source_cache(fd.get(), object.bytes, true);
    }
}

std::string read_source(const Object& object, bool drop_cache) {
    if (object.bytes > std::string{}.max_size())
        throw std::runtime_error("object is too large to materialize for BlobDB: " + object.key);
    auto fd = open_source(object);
    std::string value(static_cast<std::size_t>(object.bytes), '\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t got = ::read(fd.get(), value.data() + offset, value.size() - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) throw std::runtime_error("short read from " + object.path.string());
        offset += static_cast<std::size_t>(got);
    }
    discard_source_cache(fd.get(), object.bytes, drop_cache);
    return value;
}

[[noreturn]] void throw_goblin_error(std::string_view operation, const goblin::Error& error) {
    std::string message(operation);
    message += ": ";
    message += goblin::to_string(error.code);
    if (!error.detail.empty()) message += " (" + error.detail + ')';
    throw std::runtime_error(std::move(message));
}

void goblin_store_file(goblin::Store& store, const Object& object,
                       std::vector<std::byte>& copy_buffer, bool drop_cache) {
    auto pending = store.begin_put(object.key, object.bytes);
    if (!pending) throw_goblin_error("begin Goblin put for " + object.key, pending.error());
    auto source = open_source(object);
    std::uint64_t remaining = object.bytes;
    while (remaining != 0) {
        const std::size_t wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, copy_buffer.size()));
        ssize_t got = 0;
        do {
            got = ::read(source.get(), copy_buffer.data(), wanted);
        } while (got < 0 && errno == EINTR);
        if (got <= 0) throw std::runtime_error("short read from " + object.path.string());
        const auto written = pending->write(
            goblin::ByteView(copy_buffer.data(), static_cast<std::size_t>(got)));
        if (!written) throw_goblin_error("write Goblin value for " + object.key, written.error());
        remaining -= static_cast<std::uint64_t>(got);
    }
    discard_source_cache(source.get(), object.bytes, drop_cache);
    const auto committed = pending->commit();
    if (!committed) throw_goblin_error("commit Goblin value for " + object.key,
                                       committed.error());
}

std::pair<RunSummary, std::vector<ReadRecord>> run_goblin(
    const Args& args, const std::vector<Object>& objects,
    const std::vector<ScheduledKey>& prefetch,
    const std::vector<ScheduledKey>& schedule) {
    constexpr IoMode mode = IoMode::direct;
    const fs::path database = args.scratch / "goblin";
    remove_database(database);
    const auto prepared = goblin::Store::prepare_directory(database.string());
    if (!prepared) throw_goblin_error("prepare Goblin directory", prepared.error());
    discard_selected_source_cache(objects, prefetch, args.drop_source_cache);

    RunSummary summary{"goblin", mode};
    summary.populated_objects = prefetch.size();
    summary.populated_bytes = sum_selected_bytes(objects, prefetch);
    std::vector<ReadRecord> records;
    records.reserve(schedule.size());

    {
        goblin::StoreOptions options;
        options.ssd.dirs = {database.string()};
        options.tiers.ram_head = args.goblin_head_size;
        options.memory.total_bytes = args.goblin_large_memory;
        options.memory.small_total_bytes = args.goblin_small_memory;
        options.eviction.reclaim_interval_ms = 0;
        options.read_chunk_bytes = args.goblin_read_chunk;
        options.file_handle_cache = static_cast<unsigned>(args.goblin_file_handles);
        options.max_object_size = std::max<std::uint64_t>(
            1, largest_selected_object(objects, prefetch));
        options.direct_io = true;
        raise_file_descriptor_limit(args.goblin_file_handles);
        auto opened = goblin::Store::open(std::move(options));
        if (!opened) throw_goblin_error("open embedded Goblin store", opened.error());
        goblin::Store store = std::move(*opened);

        std::vector<std::byte> copy_buffer(kCopyBuffer);
        const std::uint64_t start = now_ns();
        for (std::size_t index = 0; index < prefetch.size(); ++index) {
            goblin_store_file(store, objects[prefetch[index].object_index], copy_buffer,
                              args.drop_source_cache);
            if ((index + 1) % 10000 == 0 || index + 1 == prefetch.size())
                std::cerr << "\r  Goblin loaded " << index + 1
                          << '/' << prefetch.size() << std::flush;
        }
        summary.population_ns = now_ns() - start;
        std::cerr << '\n';

        auto made_reader = store.make_reader();
        if (!made_reader) throw_goblin_error("create embedded Goblin reader",
                                             made_reader.error());
        goblin::StoreReader reader = std::move(*made_reader);
        const std::uint64_t read_start = now_ns();
        for (const auto& scheduled : schedule) {
            const auto& request = objects[scheduled.object_index];
            const std::uint64_t request_start = now_ns();
            std::optional<std::uint64_t> first_byte;
            std::uint64_t returned = 0;
            auto loaded = reader.load(
                request.key, [&](goblin::ByteView piece) -> goblin::Status {
                    if (!first_byte) first_byte = now_ns();
                    returned += piece.size();
                    return {};
                });
            const std::uint64_t completion = now_ns();
            ReadRecord record;
            record.time_to_end_ns = completion - request_start;
            if (!loaded && loaded.error().code == goblin::Errc::not_found) {
                record.time_to_first_byte_ns = record.time_to_end_ns;
                record.status = ReadStatus::missing;
            } else {
                if (!loaded)
                    throw_goblin_error("read embedded Goblin key " + request.key,
                                       loaded.error());
                if (returned != request.bytes || loaded->info.size != request.bytes ||
                    loaded->bytes.size() != request.bytes)
                    throw std::runtime_error("Goblin value size mismatch for " + request.key);
                record.time_to_first_byte_ns =
                    (first_byte ? *first_byte : completion) - request_start;
                record.returned_bytes = returned;
                record.status = ReadStatus::ok;
            }
            summary.read_bytes += record.returned_bytes;
            records.push_back(record);
        }
        summary.read_ns = now_ns() - read_start;
        summary.read_requests = records.size();
    }
    if (!args.keep_databases) remove_database(database);
    return {summary, std::move(records)};
}

void rocksdb_check(const rocksdb::Status& status, std::string_view operation) {
    if (!status.ok())
        throw std::runtime_error(std::string(operation) + ": " + status.ToString());
}

std::pair<RunSummary, std::vector<ReadRecord>> run_blobdb(
    const Args& args, IoMode mode, const std::vector<Object>& objects,
    const std::vector<ScheduledKey>& prefetch,
    const std::vector<ScheduledKey>& schedule) {
    const fs::path database = args.scratch / ("blobdb-" + std::string(mode_name(mode)));
    remove_database(database);
    discard_selected_source_cache(objects, prefetch, args.drop_source_cache);

    // One strict cache holds ordinary SST blocks, separated blob values, indexes, and filters.
    // Memtables are charged to it as well. Explicit shard geometry prevents a large value from
    // exceeding one tiny auto-selected shard while retaining a hard aggregate memory budget.
    auto cache = rocksdb::NewLRUCache(static_cast<std::size_t>(args.blobdb_buffer),
                                     args.blobdb_cache_shard_bits, true, 0.10);
#if ROCKSDB_MAJOR >= 7
    auto write_buffers = std::make_shared<rocksdb::WriteBufferManager>(
        static_cast<std::size_t>(args.blobdb_buffer), cache, true);
#else
    // RocksDB 6.11 (still packaged by older Ubuntu) predates the explicit allow_stall argument.
    // Its manager still charges memtables to the shared cache and triggers flushing at the limit.
    auto write_buffers = std::make_shared<rocksdb::WriteBufferManager>(
        static_cast<std::size_t>(args.blobdb_buffer), cache);
#endif
    rocksdb::BlockBasedTableOptions table;
    table.block_cache = cache;
    table.cache_index_and_filter_blocks = true;
    table.cache_index_and_filter_blocks_with_high_priority = true;

    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kNoCompression;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    options.write_buffer_manager = write_buffers;
    options.enable_blob_files = true;
    options.min_blob_size = args.blobdb_min_blob_size;
    options.blob_cache = cache;
    options.use_direct_reads = mode == IoMode::direct;
    options.use_direct_io_for_flush_and_compaction = false;
    options.allow_mmap_reads = false;
    options.max_open_files = 256;

    std::unique_ptr<rocksdb::DB> db;
#if ROCKSDB_MAJOR >= 11
    rocksdb_check(rocksdb::DB::Open(options, database.string(), &db), "open BlobDB");
#else
    rocksdb::DB* raw = nullptr;
    rocksdb_check(rocksdb::DB::Open(options, database.string(), &raw), "open BlobDB");
    db.reset(raw);
#endif

    RunSummary summary{"blobdb", mode};
    summary.populated_objects = prefetch.size();
    summary.populated_bytes = sum_selected_bytes(objects, prefetch);
    rocksdb::WriteOptions write;
    write.disableWAL = true; // both products are populated as disposable caches, not durable DBs
    const std::uint64_t population_start = now_ns();
    for (std::size_t index = 0; index < prefetch.size(); ++index) {
        const auto& object = objects[prefetch[index].object_index];
        std::string value = read_source(object, args.drop_source_cache);
        rocksdb_check(db->Put(write, object.key, value), "populate BlobDB key " + object.key);
        if ((index + 1) % 10000 == 0 || index + 1 == prefetch.size())
            std::cerr << "\r  BlobDB " << mode_name(mode) << " loaded " << index + 1
                      << '/' << prefetch.size() << std::flush;
    }
    rocksdb::FlushOptions flush;
    flush.wait = true;
    rocksdb_check(db->Flush(flush), "flush populated BlobDB");
    // Do not let a compaction that happened to overlap this particular replay contaminate its
    // latency distribution. Pause waits for currently running work and prevents new background
    // jobs until the read phase is complete.
    rocksdb_check(db->PauseBackgroundWork(), "quiesce populated BlobDB");
    summary.population_ns = now_ns() - population_start;
    std::cerr << '\n';

    std::vector<ReadRecord> records;
    records.reserve(schedule.size());
    rocksdb::ReadOptions read;
    read.fill_cache = true;
    const std::uint64_t read_start = now_ns();
    for (const auto& scheduled : schedule) {
        const auto& request = objects[scheduled.object_index];
        std::string value;
        const std::uint64_t start = now_ns();
        const rocksdb::Status status = db->Get(read, request.key, &value);
        const std::uint64_t completion = now_ns() - start;
        // DB::Get is synchronous: no part of the value is observable until the call returns.
        // BlobDB's first-byte and complete-value measurements are therefore identical.
        if (status.IsNotFound()) {
            records.push_back({completion, completion, 0, ReadStatus::missing});
        } else {
            rocksdb_check(status, "read BlobDB key " + request.key);
            if (value.size() != request.bytes)
                throw std::runtime_error("BlobDB value size mismatch for " + request.key);
            summary.read_bytes += value.size();
            records.push_back({completion, completion, value.size(), ReadStatus::ok});
        }
    }
    summary.read_ns = now_ns() - read_start;
    summary.read_requests = records.size();

    rocksdb_check(db->ContinueBackgroundWork(), "resume BlobDB background work");
    db.reset();
    cache.reset();
    write_buffers.reset();
    if (!args.keep_databases) remove_database(database);
    return {summary, std::move(records)};
}

std::string csv_quote(std::string_view input) {
    std::string result;
    result.reserve(input.size() + 2);
    result.push_back('"');
    for (const char ch : input) {
        if (ch == '"') result.push_back('"');
        result.push_back(ch);
    }
    result.push_back('"');
    return result;
}

void append_reads(std::ofstream& output, std::string_view engine, IoMode mode,
                  const std::vector<Object>& objects,
                  const std::vector<ScheduledKey>& schedule,
                  const std::vector<ReadRecord>& records) {
    if (schedule.size() != records.size()) throw std::logic_error("record count mismatch");
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& request = objects[schedule[index].object_index];
        const auto& record = records[index];
        output << engine << ',' << mode_name(mode) << ',' << index << ','
               << csv_quote(request.key) << ',' << request.bytes << ','
               << record.returned_bytes << ',' << record.time_to_first_byte_ns << ','
               << record.time_to_end_ns << ','
               << status_name(record.status) << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("failed writing reads.csv");
}

void settle_output_file(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot reopen result " + path.string() + ": " +
                                 std::strerror(errno));
    if (::fsync(fd) != 0) {
        const int saved = errno;
        ::close(fd);
        throw std::runtime_error("cannot sync result " + path.string() + ": " +
                                 std::strerror(saved));
    }
#if defined(POSIX_FADV_DONTNEED)
    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    ::close(fd);
}

void append_summary(std::ofstream& output, const RunSummary& summary) {
    output << summary.engine << ',' << mode_name(summary.mode) << ','
           << summary.population_ns << ',' << summary.populated_objects << ','
           << summary.populated_bytes << ',' << summary.read_ns << ','
           << summary.read_requests << ',' << summary.read_bytes << '\n';
    output.flush();
    if (!output) throw std::runtime_error("failed writing summary.csv");
}

void write_metadata(const Args& args, const std::vector<Object>& objects,
                    const std::vector<ScheduledKey>& prefetch,
                    const std::vector<ScheduledKey>& schedule,
                    const GoblinResidentFootprint& footprint) {
    std::ofstream output(args.output / "metadata.txt");
    if (!output) throw std::runtime_error("cannot create metadata.txt");
    output << "benchmark=bench_goblin_and_blobdb\n"
           << "selected_case=" << selected_case_name(args.selected_case) << '\n'
           << "clock=" << kBenchmarkClockName << '\n'
           << "data=" << args.data.string() << '\n'
           << "scratch=" << args.scratch.string() << '\n'
           << "prefetch=" << args.prefetch.string() << '\n'
           << "schedule=" << args.schedule.string() << '\n'
           << "goblin_interface=embedded_cpp\n"
           << "goblin_network_protocol=none\n"
           << "goblin_read_api=reused_grow_only_materialization\n"
           << "goblin_materialization_initial_capacity_bytes=0\n"
           << "goblin_small_memory_bytes=" << args.goblin_small_memory << '\n'
           << "goblin_large_memory_bytes=" << args.goblin_large_memory << '\n'
           << "goblin_head_size_bytes=" << args.goblin_head_size << '\n'
           << "goblin_read_chunk_bytes=" << args.goblin_read_chunk << '\n'
           << "goblin_file_handle_cache=" << args.goblin_file_handles << '\n'
           << "goblin_small_footprint_bytes=" << footprint.small_bytes << '\n'
           << "goblin_small_objects=" << footprint.small_objects << '\n'
           << "goblin_fixed_head_footprint_bytes=" << footprint.fixed_head_bytes << '\n'
           << "goblin_fixed_head_objects=" << footprint.fixed_head_objects << '\n'
           << "goblin_disk_objects="
           << goblin_disk_object_count(objects, prefetch, args.goblin_head_size) << '\n'
           << "blobdb_buffer_bytes=" << args.blobdb_buffer << '\n'
           << "blobdb_min_blob_size_bytes=" << args.blobdb_min_blob_size << '\n'
           << "blobdb_cache_shard_bits=" << args.blobdb_cache_shard_bits << '\n'
           << "blobdb_cache_shards=" << (std::uint64_t{1} << args.blobdb_cache_shard_bits) << '\n'
           << "blobdb_cache_bytes_per_shard="
           << (args.blobdb_buffer /
               (std::uint64_t{1} << args.blobdb_cache_shard_bits)) << '\n'
           << "largest_prefetched_object_bytes="
           << largest_selected_object(objects, prefetch) << '\n'
           << "rocksdb_version=" << rocksdb_version() << '\n'
           << "blobdb_background_work_during_reads=paused\n"
           << "source_objects=" << objects.size() << '\n'
           << "source_bytes=" << sum_bytes(objects) << '\n'
           << "prefetched_objects=" << prefetch.size() << '\n'
           << "prefetched_bytes=" << sum_selected_bytes(objects, prefetch) << '\n'
           << "scheduled_requests=" << schedule.size() << '\n'
           << "source_cache_advice="
           << (args.drop_source_cache
                   ? "POSIX_FADV_DONTNEED before each population and after every source read"
                   : "retained") << '\n';
}

void print_summary(const RunSummary& summary) {
    const double population_seconds = static_cast<double>(summary.population_ns) / 1e9;
    const double read_seconds = static_cast<double>(summary.read_ns) / 1e9;
    const double population_mib = static_cast<double>(summary.populated_bytes) / MiB;
    const double read_mib = static_cast<double>(summary.read_bytes) / MiB;
    std::cerr << "  " << summary.engine << ' ' << mode_name(summary.mode)
              << ": populated " << summary.populated_objects << " objects in "
              << population_seconds << " s ("
              << (population_seconds == 0.0 ? 0.0 : population_mib / population_seconds)
              << " MiB/s); read " << summary.read_requests << " requests in "
              << read_seconds << " s ("
              << (read_seconds == 0.0 ? 0.0 : read_mib / read_seconds) << " MiB/s)\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        args.data = canonical_existing(args.data, "--data");
        args.prefetch = canonical_existing(args.prefetch, "--prefetch");
        args.schedule = canonical_existing(args.schedule, "--schedule");
        ensure_directory(args.scratch);
        args.scratch = canonical_existing(args.scratch, "--scratch");
        ensure_directory(args.output);
        args.output = canonical_existing(args.output, "--output");
        if (!fs::is_directory(args.data)) throw std::runtime_error("--data is not a directory");
        if (!fs::is_regular_file(args.prefetch))
            throw std::runtime_error("--prefetch is not a regular file");
        if (!fs::is_regular_file(args.schedule))
            throw std::runtime_error("--schedule is not a regular file");
        if (path_prefix(args.data, args.scratch) || path_prefix(args.scratch, args.data))
            throw std::runtime_error("--data and --scratch must not contain one another");
        if (path_prefix(args.data, args.output) || path_prefix(args.output, args.data) ||
            path_prefix(args.scratch, args.output) || path_prefix(args.output, args.scratch))
            throw std::runtime_error(
                "--output must be separate from both --data and --scratch");

        std::cerr << "Inventorying " << args.data << " ...\n";
        const auto objects = inventory(args.data);
        const auto prefetch = load_key_list(args.prefetch, objects, "prefetch", true);
        const auto schedule = load_key_list(args.schedule, objects, "schedule", false);
        validate_schedule_is_prefetched(prefetch, schedule, objects);
        const auto footprint =
            goblin_resident_footprint(objects, prefetch, args.goblin_head_size);
        validate_goblin_resident_capacity(args, footprint);
        const auto goblin_disk_objects =
            goblin_disk_object_count(objects, prefetch, args.goblin_head_size);
        if ((args.selected_case == SelectedCase::all ||
             args.selected_case == SelectedCase::goblin) &&
            args.goblin_file_handles < goblin_disk_objects)
            throw std::runtime_error(
                "--goblin-file-handles is smaller than the " +
                std::to_string(goblin_disk_objects) +
                " disk-backed prefetched objects; choose the next power of two to avoid churn");
        if (args.selected_case != SelectedCase::goblin)
            validate_blobdb_cache_geometry(args, objects, prefetch);
        std::cerr << "Found " << objects.size() << " objects (" << sum_bytes(objects)
                  << " bytes); prefetch has " << prefetch.size() << " objects ("
                  << sum_selected_bytes(objects, prefetch) << " bytes); schedule has "
                  << schedule.size() << " reads.\n"
                  << "Goblin resident footprint: " << footprint.small_bytes
                  << " small-pool bytes and " << footprint.fixed_head_bytes
                  << " fixed-head bytes at " << args.goblin_head_size << " bytes/head.\n"
                  << "Results: " << args.output << '\n';
        write_metadata(args, objects, prefetch, schedule, footprint);

        std::ofstream reads(args.output / "reads.csv");
        std::ofstream summaries(args.output / "summary.csv");
        if (!reads || !summaries) throw std::runtime_error("cannot create result CSV files");
        reads << "engine,io_mode,sequence,key,expected_bytes,returned_bytes,"
                 "time_to_first_byte_ns,time_to_end_ns,status\n";
        summaries << "engine,io_mode,population_ns,populated_objects,populated_bytes,"
                     "read_phase_ns,read_requests,read_bytes\n";

        if (args.selected_case == SelectedCase::all ||
            args.selected_case == SelectedCase::goblin) {
            std::cerr << "\n=== Goblin Store (O_DIRECT) ===\n";
            auto [goblin_summary, goblin_records] =
                run_goblin(args, objects, prefetch, schedule);
            append_reads(reads, "goblin", IoMode::direct, objects, schedule, goblin_records);
            append_summary(summaries, goblin_summary);
            settle_output_file(args.output / "reads.csv");
            settle_output_file(args.output / "summary.csv");
            print_summary(goblin_summary);
        }

        for (const IoMode mode : {IoMode::buffered, IoMode::direct}) {
            const bool selected = args.selected_case == SelectedCase::all ||
                (mode == IoMode::buffered &&
                 args.selected_case == SelectedCase::blobdb_buffered) ||
                (mode == IoMode::direct &&
                 args.selected_case == SelectedCase::blobdb_direct);
            if (!selected) continue;
            std::cerr << "\n=== " << mode_name(mode) << " I/O: BlobDB ===\n";
            auto [blob_summary, blob_records] =
                run_blobdb(args, mode, objects, prefetch, schedule);
            append_reads(reads, "blobdb", mode, objects, schedule, blob_records);
            append_summary(summaries, blob_summary);
            settle_output_file(args.output / "reads.csv");
            settle_output_file(args.output / "summary.csv");
            print_summary(blob_summary);
        }

        std::cerr << "\nComplete. Artifacts: " << args.output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
