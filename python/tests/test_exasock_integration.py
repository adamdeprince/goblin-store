import io
import os
import secrets

import pytest

import goblin_store


ADDRESS = os.getenv("GOBLIN_STORE_EXASOCK_ADDRESS")
PORT = int(os.getenv("GOBLIN_STORE_EXASOCK_PORT", "11211"))

pytestmark = pytest.mark.skipif(
    not ADDRESS,
    reason="set GOBLIN_STORE_EXASOCK_ADDRESS and run pytest through exasock",
)


def test_exasock_binary_memcache_interoperability():
    assert goblin_store.exasock_available(), "wheel was not built with ExaSock support"
    assert goblin_store.exasock_active(), "pytest was not started through the exasock launcher"

    key = f"python-exasock-{secrets.token_hex(8)}"
    with goblin_store.Client(ADDRESS, PORT, transport="exasock") as client:
        for size in (0, 1, 191, 192, 193, 256 * 1024, 1024 * 1024):
            value = bytes((index * 37) & 0xFF for index in range(size))
            assert client.set(key, value, flags=17)
            assert client.get(key) == value

            item = client.gets(key)
            assert item is not None
            assert item.value == value
            assert item.flags == 17
            assert item.cas is not None

            replacement = value + b"next"
            assert client.cas(key, replacement, item.cas, flags=23)
            output = io.BytesIO()
            info = client.get_into(key, output, with_cas=True, chunk_bytes=4093)
            assert info is not None
            assert info.size == len(replacement)
            assert info.flags == 23
            assert info.cas is not None
            assert output.getvalue() == replacement

        assert client.delete(key)
        assert client.get(key) is None
