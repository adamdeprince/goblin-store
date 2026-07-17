#include "goblin/store/client.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nb = nanobind;

namespace {

using goblin::client::Client;
using goblin::client::DeleteResult;
using goblin::client::ExasockOptions;
using goblin::client::Item;
using goblin::client::ItemInfo;
using goblin::client::Options;
using goblin::client::StoreResult;

std::string_view bytes_view(const nb::bytes& value) noexcept {
    return {PyBytes_AS_STRING(value.ptr()), static_cast<std::size_t>(PyBytes_GET_SIZE(value.ptr()))};
}

class BufferView {
public:
    explicit BufferView(nb::handle object) {
        if (PyObject_GetBuffer(object.ptr(), &view_, PyBUF_CONTIG_RO) != 0)
            throw nb::python_error();
        active_ = true;
        if (view_.len < 0) {
            PyBuffer_Release(&view_);
            active_ = false;
            throw std::invalid_argument("Python buffer has a negative length");
        }
        // Only an actual bytes object is known to remain immutable while the
        // GIL is released. Even a read-only memoryview can alias a bytearray
        // that another Python thread mutates through a separate view.
        if (!PyBytes_CheckExact(object.ptr())) {
            try {
                if (view_.len != 0) {
                    if (!view_.buf)
                        throw std::invalid_argument("Python buffer has no storage");
                    owned_.assign(static_cast<const char*>(view_.buf),
                                  static_cast<std::size_t>(view_.len));
                }
            } catch (...) {
                PyBuffer_Release(&view_);
                active_ = false;
                throw;
            }
            PyBuffer_Release(&view_);
            active_ = false;
        }
    }
    BufferView(const BufferView&) = delete;
    BufferView& operator=(const BufferView&) = delete;
    ~BufferView() {
        if (active_) PyBuffer_Release(&view_);
    }
    std::string_view get() const noexcept {
        if (!active_) return owned_;
        return {static_cast<const char*>(view_.buf), static_cast<std::size_t>(view_.len)};
    }

private:
    Py_buffer view_{};
    std::string owned_;
    bool active_ = false;
};

nb::object item_value(const std::optional<Item>& item) {
    if (!item) return nb::none();
    return nb::bytes(item->value.data(), item->value.size());
}

nb::object item_tuple(const std::optional<Item>& item) {
    if (!item) return nb::none();
    nb::object cas = item->cas ? nb::object(nb::int_(*item->cas)) : nb::none();
    return nb::make_tuple(nb::bytes(item->value.data(), item->value.size()), item->flags,
                          item->size, std::move(cas));
}

nb::object info_tuple(const std::optional<ItemInfo>& item) {
    if (!item) return nb::none();
    nb::object cas = item->cas ? nb::object(nb::int_(*item->cas)) : nb::none();
    return nb::make_tuple(item->flags, item->size, std::move(cas));
}

class PythonClient {
public:
    PythonClient(const std::string& address, std::uint16_t port, std::uint64_t ring_bytes,
                 long connect_timeout_ms, long operation_timeout_ms,
                 std::uint64_t max_value_bytes, std::uint32_t bulk_window_bytes,
                 std::uint16_t bulk_window_count, const std::string& transport) {
        if (connect_timeout_ms < 0 || operation_timeout_ms < 0)
            throw std::invalid_argument("timeouts must not be negative");
        if (transport == "rdma") {
            Options options;
            options.address = address;
            options.port = port;
            options.ring_bytes = ring_bytes;
            options.bulk_window_bytes = bulk_window_bytes;
            options.bulk_window_count = bulk_window_count;
            options.connect_timeout = std::chrono::milliseconds(connect_timeout_ms);
            options.operation_timeout = std::chrono::milliseconds(operation_timeout_ms);
            options.max_value_bytes = max_value_bytes;
            client_ = std::make_unique<Client>(Client::connect(options));
            return;
        }
        if (transport == "exasock") {
            if (ring_bytes != 64 * 1024 || bulk_window_bytes != 256 * 1024 ||
                bulk_window_count != 4) {
                throw std::invalid_argument(
                    "ring_bytes and bulk-window options apply only to RDMA");
            }
            ExasockOptions options;
            options.address = address;
            options.port = port;
            options.connect_timeout = std::chrono::milliseconds(connect_timeout_ms);
            options.operation_timeout = std::chrono::milliseconds(operation_timeout_ms);
            options.max_value_bytes = max_value_bytes;
            client_ = std::make_unique<Client>(Client::connect_exasock(options));
            return;
        }
        throw std::invalid_argument("transport must be 'rdma' or 'exasock'");
    }

    nb::object get(const nb::bytes& key) {
        const std::string_view key_bytes = bytes_view(key);
        std::optional<Item> item;
        {
            nb::gil_scoped_release release;
            item = client_->get(key_bytes);
        }
        return item_value(item);
    }

    nb::object gets(const nb::bytes& key) {
        const std::string_view key_bytes = bytes_view(key);
        std::optional<Item> item;
        {
            nb::gil_scoped_release release;
            item = client_->gets(key_bytes);
        }
        return item_tuple(item);
    }

    StoreResult set(const nb::bytes& key, nb::handle value, std::uint32_t flags,
                    std::uint32_t exptime) {
        BufferView buffer(value);
        const std::string_view key_bytes = bytes_view(key);
        const std::string_view value_bytes = buffer.get();
        nb::gil_scoped_release release;
        return client_->set(key_bytes, value_bytes, flags, exptime);
    }

    StoreResult add(const nb::bytes& key, nb::handle value, std::uint32_t flags,
                    std::uint32_t exptime) {
        BufferView buffer(value);
        const std::string_view key_bytes = bytes_view(key);
        const std::string_view value_bytes = buffer.get();
        nb::gil_scoped_release release;
        return client_->add(key_bytes, value_bytes, flags, exptime);
    }

    StoreResult replace(const nb::bytes& key, nb::handle value, std::uint32_t flags,
                        std::uint32_t exptime) {
        BufferView buffer(value);
        const std::string_view key_bytes = bytes_view(key);
        const std::string_view value_bytes = buffer.get();
        nb::gil_scoped_release release;
        return client_->replace(key_bytes, value_bytes, flags, exptime);
    }

    StoreResult compare_exchange(const nb::bytes& key, nb::handle value, std::uint64_t cas,
                                 std::uint32_t flags, std::uint32_t exptime) {
        BufferView buffer(value);
        const std::string_view key_bytes = bytes_view(key);
        const std::string_view value_bytes = buffer.get();
        nb::gil_scoped_release release;
        return client_->compare_exchange(key_bytes, value_bytes, cas, flags, exptime);
    }

    DeleteResult erase(const nb::bytes& key) {
        const std::string_view key_bytes = bytes_view(key);
        nb::gil_scoped_release release;
        return client_->erase(key_bytes);
    }

    std::string version() {
        nb::gil_scoped_release release;
        return client_->version();
    }

    nb::dict stats() {
        std::unordered_map<std::string, std::string> values;
        {
            nb::gil_scoped_release release;
            values = client_->stats();
        }
        nb::dict result;
        for (const auto& [key, value] : values)
            result[nb::str(key.data(), key.size())] = nb::str(value.data(), value.size());
        return result;
    }

    nb::object get_into(const nb::bytes& key, nb::object writer, bool with_cas,
                        std::size_t chunk_bytes) {
        if (chunk_bytes == 0) throw std::invalid_argument("chunk_bytes must be greater than zero");
        const std::string_view key_bytes = bytes_view(key);
        std::string buffered;
        buffered.reserve(chunk_bytes);
        std::optional<ItemInfo> info;
        auto flush = [&] {
            if (buffered.empty()) return;
            nb::gil_scoped_acquire acquire;
            nb::object count = writer.attr("write")(nb::bytes(buffered.data(), buffered.size()));
            if (!PyLong_Check(count.ptr()) || PyBool_Check(count.ptr()))
                throw std::runtime_error(
                    "writer.write() must return the exact integer byte count");
            if (nb::cast<std::size_t>(count) != buffered.size())
                throw std::runtime_error("writer accepted only part of a Goblin Store value chunk");
            buffered.clear();
        };
        {
            nb::gil_scoped_release release;
            info = client_->get_to(key_bytes, [&](std::string_view bytes) {
                while (!bytes.empty()) {
                    const std::size_t available = chunk_bytes - buffered.size();
                    const std::size_t take = std::min(available, bytes.size());
                    buffered.append(bytes.data(), take);
                    bytes.remove_prefix(take);
                    if (buffered.size() == chunk_bytes) flush();
                }
            }, with_cas);
            flush();
        }
        return info_tuple(info);
    }

    void close() {
        nb::gil_scoped_release release;
        client_->close();
    }

private:
    std::unique_ptr<Client> client_;
};

} // namespace

NB_MODULE(_goblin_store, module) {
    module.doc() = "Native RDMA and ExaSock memcache client for Goblin Store";

    auto error = nb::exception<goblin::client::Error>(module, "Error");
    nb::exception<goblin::client::ConnectionError>(module, "ConnectionError", error);
    nb::exception<goblin::client::TimeoutError>(module, "TimeoutError", error);
    nb::exception<goblin::client::ProtocolError>(module, "ProtocolError", error);
    nb::exception<goblin::client::ServerError>(module, "ServerError", error);
    nb::exception<goblin::client::ValueTooLargeError>(module, "ValueTooLargeError", error);

    nb::enum_<StoreResult>(module, "StoreResult")
        .value("STORED", StoreResult::stored)
        .value("NOT_STORED", StoreResult::not_stored)
        .value("EXISTS", StoreResult::exists)
        .value("NOT_FOUND", StoreResult::not_found);
    nb::enum_<DeleteResult>(module, "DeleteResult")
        .value("DELETED", DeleteResult::deleted)
        .value("NOT_FOUND", DeleteResult::not_found);

    nb::class_<PythonClient>(module, "_Client")
        .def(nb::init<const std::string&, std::uint16_t, std::uint64_t, long, long,
                      std::uint64_t, std::uint32_t, std::uint16_t,
                      const std::string&>(),
             nb::arg("address"), nb::arg("port") = 11211,
             nb::arg("ring_bytes") = 64 * 1024,
             nb::arg("connect_timeout_ms") = 5000,
             nb::arg("operation_timeout_ms") = 30000,
             nb::arg("max_value_bytes") = 0,
             nb::arg("bulk_window_bytes") = 256 * 1024,
             nb::arg("bulk_window_count") = 4,
             nb::arg("transport") = "rdma",
             nb::call_guard<nb::gil_scoped_release>())
        .def("get", &PythonClient::get, nb::arg("key"))
        .def("gets", &PythonClient::gets, nb::arg("key"))
        .def("set", &PythonClient::set, nb::arg("key"), nb::arg("value"),
             nb::arg("flags") = 0, nb::arg("exptime") = 0)
        .def("add", &PythonClient::add, nb::arg("key"), nb::arg("value"),
             nb::arg("flags") = 0, nb::arg("exptime") = 0)
        .def("replace", &PythonClient::replace, nb::arg("key"), nb::arg("value"),
             nb::arg("flags") = 0, nb::arg("exptime") = 0)
        .def("compare_exchange", &PythonClient::compare_exchange, nb::arg("key"),
             nb::arg("value"), nb::arg("cas"), nb::arg("flags") = 0,
             nb::arg("exptime") = 0)
        .def("erase", &PythonClient::erase, nb::arg("key"))
        .def("version", &PythonClient::version)
        .def("stats", &PythonClient::stats)
        .def("get_into", &PythonClient::get_into, nb::arg("key"), nb::arg("writer"),
             nb::arg("with_cas") = false, nb::arg("chunk_bytes") = 256 * 1024)
        .def("close", &PythonClient::close);

    module.def("rdma_available", &goblin::client::rdma_available);
    module.def("exasock_available", &goblin::client::exasock_available);
    module.def("exasock_active", &goblin::client::exasock_active);
}
