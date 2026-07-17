from dataclasses import FrozenInstanceError

import pytest

import goblin_store


def test_public_metadata_types_are_immutable():
    info = goblin_store.ItemInfo(flags=7, size=12, cas=99)
    item = goblin_store.Item(flags=7, size=3, cas=99, value=b"abc")
    assert (info.flags, info.size, info.cas) == (7, 12, 99)
    assert item.value == b"abc"
    with pytest.raises(FrozenInstanceError):
        info.flags = 8


def test_key_and_value_validation_happens_before_connecting():
    assert goblin_store._key("hello") == b"hello"
    assert goblin_store._key(memoryview(b"hello")) == b"hello"
    assert goblin_store._value(bytearray(b"value")) == bytearray(b"value")
    contiguous = memoryview(b"value")
    assert goblin_store._value(contiguous) is contiguous
    assert goblin_store._value(memoryview(b"abcdef")[::2]) == b"ace"
    with pytest.raises(TypeError):
        goblin_store._key(123)
    with pytest.raises(TypeError):
        goblin_store._value(object())


def test_extension_reports_native_rdma():
    assert isinstance(goblin_store.rdma_available(), bool)
    assert isinstance(goblin_store.exasock_available(), bool)
    assert isinstance(goblin_store.exasock_active(), bool)
    if goblin_store.exasock_active():
        assert goblin_store.exasock_available() is True


def test_native_failures_share_the_public_error_base():
    errors = (
        goblin_store.ConnectionError,
        goblin_store.TimeoutError,
        goblin_store.ProtocolError,
        goblin_store.ServerError,
        goblin_store.ValueTooLargeError,
    )
    assert all(issubclass(error, goblin_store.Error) for error in errors)


def test_bulk_window_options_are_forwarded_to_native_client(monkeypatch):
    calls = []

    class FakeNativeClient:
        def __init__(self, *args):
            calls.append(args)

        def close(self):
            pass

    monkeypatch.setattr(goblin_store._goblin_store, "_Client", FakeNativeClient)
    client = goblin_store.Client(
        "192.0.2.7",
        12000,
        ring_bytes=32768,
        bulk_window_bytes=1024 * 1024,
        bulk_window_count=8,
    )
    client.close()

    assert len(calls) == 1
    assert calls[0][0:3] == ("192.0.2.7", 12000, 32768)
    assert calls[0][-3:-1] == (1024 * 1024, 8)
    assert calls[0][-1] == "rdma"


def test_exasock_transport_is_explicitly_forwarded(monkeypatch):
    calls = []

    class FakeNativeClient:
        def __init__(self, *args):
            calls.append(args)

        def close(self):
            pass

    monkeypatch.setattr(goblin_store._goblin_store, "_Client", FakeNativeClient)
    client = goblin_store.Client("192.0.2.8", transport="exasock")
    assert "transport='exasock'" in repr(client)
    client.close()

    assert len(calls) == 1
    assert calls[0][-1] == "exasock"


def test_exasock_rejects_rdma_only_options_before_connecting(monkeypatch):
    class UnexpectedNativeClient:
        def __init__(self, *args):
            raise AssertionError("RDMA-only options reached the native client")

    monkeypatch.setattr(goblin_store._goblin_store, "_Client", UnexpectedNativeClient)
    with pytest.raises(ValueError, match="only to RDMA"):
        goblin_store.Client("192.0.2.8", transport="exasock", ring_bytes=4096)


def test_unknown_transport_is_rejected_before_connecting(monkeypatch):
    class UnexpectedNativeClient:
        def __init__(self, *args):
            raise AssertionError("unknown transport reached the native client")

    monkeypatch.setattr(goblin_store._goblin_store, "_Client", UnexpectedNativeClient)
    with pytest.raises(ValueError, match="rdma.*exasock"):
        goblin_store.Client("192.0.2.8", transport="tcp")


@pytest.mark.parametrize(
    ("window_bytes", "window_count"),
    (
        (0, 4),
        (2048, 4),
        (6144, 4),
        (4096, 0),
        (4096, 65536),
        (1 << 30, 2),
    ),
)
def test_python_bulk_window_geometry_is_validated_before_connecting(
    monkeypatch, window_bytes, window_count
):
    class UnexpectedNativeClient:
        def __init__(self, *args):
            raise AssertionError("invalid geometry reached the native client")

    monkeypatch.setattr(goblin_store._goblin_store, "_Client", UnexpectedNativeClient)
    with pytest.raises(ValueError):
        goblin_store.Client(
            "192.0.2.7",
            bulk_window_bytes=window_bytes,
            bulk_window_count=window_count,
        )


@pytest.mark.parametrize(
    ("window_bytes", "window_count"),
    ((2048, 4), (6144, 4), (4096, 0), (1 << 30, 2)),
)
def test_native_bulk_window_geometry_is_validated_before_rdma_connect(
    window_bytes, window_count
):
    with pytest.raises(ValueError, match="RDMA bulk windows"):
        goblin_store._goblin_store._Client(
            "192.0.2.7",
            connect_timeout_ms=0,
            bulk_window_bytes=window_bytes,
            bulk_window_count=window_count,
        )


def test_native_exasock_rejects_rdma_only_options():
    with pytest.raises(ValueError, match="only to RDMA"):
        goblin_store._goblin_store._Client(
            "192.0.2.8",
            ring_bytes=4096,
            transport="exasock",
        )


def test_bulk_window_geometry_accepts_its_smallest_and_largest_counts():
    goblin_store._validate_bulk_geometry(4096, 1)
    goblin_store._validate_bulk_geometry(4096, 65535)


@pytest.mark.parametrize(("window_bytes", "window_count"), ((True, 4), (4096, False)))
def test_bulk_window_geometry_rejects_boolean_integers(window_bytes, window_count):
    with pytest.raises(TypeError):
        goblin_store._validate_bulk_geometry(window_bytes, window_count)
