import io
import os
import secrets

import pytest

import goblin_store


ADDRESS = os.getenv("GOBLIN_STORE_RDMA_ADDRESS")
PORT = int(os.getenv("GOBLIN_STORE_RDMA_PORT", "11211"))

pytestmark = pytest.mark.skipif(
    not ADDRESS,
    reason="set GOBLIN_STORE_RDMA_ADDRESS for a configured native RDMA server",
)


class RecordingWriter:
    def __init__(self):
        self.chunks = []

    def write(self, data):
        self.chunks.append(bytes(data))
        return len(data)


class FixedResultWriter:
    def __init__(self, result):
        self.result = result

    def write(self, data):
        return self.result


def test_binary_set_get_gets_cas_delete_and_streaming():
    key = f"python-rdma-{secrets.token_hex(8)}"
    with goblin_store.Client(ADDRESS, PORT) as client:
        for size in (0, 1, 191, 192, 193, 1024 * 1024):
            value = bytes((i * 37) & 0xFF for i in range(size))
            assert client.set(key, value, flags=17)
            assert client.get(key) == value

            item = client.gets(key)
            assert item is not None
            assert item.value == value
            assert item.flags == 17
            assert item.cas is not None

            updated = value + b"next"
            assert client.cas(key, updated, item.cas, flags=23)
            destination = io.BytesIO()
            info = client.get_into(key, destination, with_cas=True)
            assert info is not None
            assert destination.getvalue() == updated
            assert info.flags == 23
            assert info.cas is not None

        assert client.delete(key)
        assert client.get(key) is None


def test_get_into_enforces_chunk_boundaries_and_writer_results():
    key = f"python-rdma-writer-{secrets.token_hex(8)}"
    value = bytes((i * 19) & 0xFF for i in range(10_003))
    try:
        with goblin_store.Client(ADDRESS, PORT) as client:
            assert client.set(key, value)
            writer = RecordingWriter()
            info = client.get_into(key, writer, chunk_bytes=37)
            assert info is not None
            assert info.size == len(value)
            assert b"".join(writer.chunks) == value
            assert writer.chunks
            assert all(0 < len(chunk) <= 37 for chunk in writer.chunks)

        for invalid_result in (None, True, 0):
            with goblin_store.Client(ADDRESS, PORT) as client:
                with pytest.raises(RuntimeError):
                    client.get_into(
                        key,
                        FixedResultWriter(invalid_result),
                        chunk_bytes=4096,
                    )
    finally:
        with goblin_store.Client(ADDRESS, PORT) as client:
            client.delete(key)
