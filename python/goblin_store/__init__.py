"""Native InfiniBand client for Goblin Store's memcache protocol."""

from __future__ import annotations

from dataclasses import dataclass
from typing import BinaryIO, Union

from . import _goblin_store
from ._goblin_store import (
    ConnectionError,
    DeleteResult,
    Error,
    ProtocolError,
    ServerError,
    StoreResult,
    TimeoutError,
    ValueTooLargeError,
)

__version__ = "0.1.0"
__all__ = [
    "Client",
    "Item",
    "ItemInfo",
    "StoreResult",
    "DeleteResult",
    "Error",
    "ConnectionError",
    "TimeoutError",
    "ProtocolError",
    "ServerError",
    "ValueTooLargeError",
    "rdma_available",
]

Key = Union[str, bytes, bytearray, memoryview]
Value = Union[str, bytes, bytearray, memoryview]


@dataclass(frozen=True, slots=True)
class ItemInfo:
    flags: int
    size: int
    cas: int | None = None


@dataclass(frozen=True, slots=True)
class Item(ItemInfo):
    value: bytes = b""


def _key(value: Key) -> bytes:
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, bytes):
        return value
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value)
    raise TypeError("memcache keys must be str or bytes-like")


def _value(value: Value):
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, (bytes, bytearray)):
        return value
    if isinstance(value, memoryview):
        return value if value.c_contiguous else value.tobytes()
    raise TypeError("memcache values must be str or bytes-like")


def _validate_bulk_geometry(window_bytes: int, window_count: int) -> None:
    if (not isinstance(window_bytes, int) or isinstance(window_bytes, bool) or
            not isinstance(window_count, int) or isinstance(window_count, bool)):
        raise TypeError("bulk window size and count must be integers")
    if window_bytes < 4096 or window_bytes & (window_bytes - 1):
        raise ValueError("bulk_window_bytes must be a power of two of at least 4096")
    if not 1 <= window_count <= 65535:
        raise ValueError("bulk_window_count must be between 1 and 65535")
    if 2 * window_bytes * window_count > (1 << 32) - 1:
        raise ValueError("combined receive and staging bulk windows must be smaller than 4 GiB")


def rdma_available() -> bool:
    """Return whether this extension was built with the native verbs transport."""
    return bool(_goblin_store.rdma_available())


class Client:
    """One ordered memcache connection over a native one-sided RDMA ring.

    Calls are serialized per instance. Use one Client per worker/thread when
    concurrent requests should occupy independent queue pairs.
    """

    def __init__(
        self,
        address: str,
        port: int = 11211,
        *,
        ring_bytes: int = 64 * 1024,
        connect_timeout: float = 5.0,
        timeout: float = 30.0,
        max_value_bytes: int = 0,
        bulk_window_bytes: int = 256 * 1024,
        bulk_window_count: int = 4,
    ) -> None:
        if connect_timeout < 0 or timeout < 0:
            raise ValueError("timeouts must not be negative")
        _validate_bulk_geometry(bulk_window_bytes, bulk_window_count)
        self._address = address
        self._port = port
        self._client = _goblin_store._Client(
            address,
            port,
            ring_bytes,
            round(connect_timeout * 1000),
            round(timeout * 1000),
            max_value_bytes,
            bulk_window_bytes,
            bulk_window_count,
        )

    def __repr__(self) -> str:
        return f"Client(address={self._address!r}, port={self._port})"

    def get(self, key: Key) -> bytes | None:
        return self._client.get(_key(key))

    def gets(self, key: Key) -> Item | None:
        result = self._client.gets(_key(key))
        if result is None:
            return None
        value, flags, size, cas = result
        return Item(flags=flags, size=size, cas=cas, value=value)

    def get_into(
        self,
        key: Key,
        writer: BinaryIO,
        *,
        with_cas: bool = False,
        chunk_bytes: int = 256 * 1024,
    ) -> ItemInfo | None:
        """Stream into chunks no larger than ``chunk_bytes``.

        ``writer.write`` must return an integer exactly equal to the number of
        bytes passed; ``None``, ``bool``, and partial writes are errors.
        """
        result = self._client.get_into(_key(key), writer, with_cas, chunk_bytes)
        if result is None:
            return None
        flags, size, cas = result
        return ItemInfo(flags=flags, size=size, cas=cas)

    def set_result(self, key: Key, value: Value, *, flags: int = 0,
                   expire: int = 0) -> StoreResult:
        return self._client.set(_key(key), _value(value), flags, expire)

    def set(self, key: Key, value: Value, *, flags: int = 0, expire: int = 0) -> bool:
        return self.set_result(key, value, flags=flags, expire=expire) == StoreResult.STORED

    def add_result(self, key: Key, value: Value, *, flags: int = 0,
                   expire: int = 0) -> StoreResult:
        return self._client.add(_key(key), _value(value), flags, expire)

    def add(self, key: Key, value: Value, *, flags: int = 0, expire: int = 0) -> bool:
        return self.add_result(key, value, flags=flags, expire=expire) == StoreResult.STORED

    def replace_result(self, key: Key, value: Value, *, flags: int = 0,
                       expire: int = 0) -> StoreResult:
        return self._client.replace(_key(key), _value(value), flags, expire)

    def replace(self, key: Key, value: Value, *, flags: int = 0,
                expire: int = 0) -> bool:
        return self.replace_result(key, value, flags=flags, expire=expire) == StoreResult.STORED

    def cas_result(self, key: Key, value: Value, cas: int, *, flags: int = 0,
                   expire: int = 0) -> StoreResult:
        return self._client.compare_exchange(_key(key), _value(value), cas, flags, expire)

    def cas(self, key: Key, value: Value, cas: int, *, flags: int = 0,
            expire: int = 0) -> bool:
        return self.cas_result(key, value, cas, flags=flags, expire=expire) == StoreResult.STORED

    def delete_result(self, key: Key) -> DeleteResult:
        return self._client.erase(_key(key))

    def delete(self, key: Key) -> bool:
        return self.delete_result(key) == DeleteResult.DELETED

    def version(self) -> str:
        return self._client.version()

    def stats(self) -> dict[str, str]:
        return self._client.stats()

    def close(self) -> None:
        self._client.close()

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
