import sys

import pytest

import libvalkey

testdata = [
    # all_types
    (
        (
            "HSET",
            "foo",
            "key",
            "value1",
            b"key_b",
            b"bytes str",
            "key_mv",
            memoryview(b"bytes str"),
            b"key_i",
            67,
            "key_f",
            3.14159265359,
        ),
        b"*12\r\n$4\r\nHSET\r\n$3\r\nfoo\r\n$3\r\nkey\r\n$6\r\nvalue1\r\n$5\r\nkey_b\r\n$9\r\nbytes str\r\n$6\r\nkey_mv\r\n$9\r\nbytes str\r\n$5\r\nkey_i\r\n$2\r\n67\r\n$5\r\nkey_f\r\n$13\r\n3.14159265359\r\n",
    ),
    # spaces_in_cmd
    (
        (
            "COMMAND",
            "GETKEYS",
            "EVAL",
            "return {KEYS[1],KEYS[2],ARGV[1],ARGV[2]}",
            2,
            "key1",
            "key2",
            "first",
            "second",
        ),
        b"*9\r\n$7\r\nCOMMAND\r\n$7\r\nGETKEYS\r\n$4\r\nEVAL\r\n$40\r\nreturn {KEYS[1],KEYS[2],ARGV[1],ARGV[2]}\r\n$1\r\n2\r\n$4\r\nkey1\r\n$4\r\nkey2\r\n$5\r\nfirst\r\n$6\r\nsecond\r\n",
    ),
    # null_chars
    (
        ("SET", "a", bytes(b"\xaa\x00\xffU")),
        b"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$4\r\n\xaa\x00\xffU\r\n",
    ),
    # encoding
    (
        ("SET", "a", "йדב문諺"),
        b"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$12\r\n\xd0\xb9\xd7\x93\xd7\x91\xeb\xac\xb8\xe8\xab\xba\r\n",
    ),
    # big_int
    (
        ("SET", "a", 2**128),
        b"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$39\r\n340282366920938463463374607431768211456\r\n",
    ),
]

testdata_ids = [
    "all_types",
    "spaces_in_cmd",
    "null_chars",
    "encoding",
    "big_int",
]


@pytest.mark.parametrize("cmd,expected_packed_cmd", testdata, ids=testdata_ids)
def test_basic(cmd, expected_packed_cmd):
    packed_cmd = libvalkey.pack_command(cmd)
    assert packed_cmd == expected_packed_cmd


def test_wrong_type():
    with pytest.raises(TypeError):
        libvalkey.pack_command(("HSET", "foo", True))

    class Bar:
        def __init__(self) -> None:
            self.key = "key"
            self.value = 36

    with pytest.raises(TypeError):
        libvalkey.pack_command(("HSET", "foo", Bar()))


def test_empty_command():
    assert libvalkey.pack_command(()) == b"*0\r\n"


def test_empty_items():
    assert (
        libvalkey.pack_command(("SET", b"", "", memoryview(b"")))
        == b"*4\r\n$3\r\nSET\r\n$0\r\n\r\n$0\r\n\r\n$0\r\n\r\n"
    )


@pytest.mark.parametrize("cmd", [["SET", "a"], None, "SET", 1, {"SET": "a"}])
def test_argument_must_be_a_tuple(cmd):
    with pytest.raises(TypeError):
        libvalkey.pack_command(cmd)


def test_released_memoryview():
    view = memoryview(b"abc")
    view.release()
    with pytest.raises(ValueError):
        libvalkey.pack_command(("SET", "a", view))


def test_memoryview_that_is_not_contiguous():
    # Its content is b"ace", which is not what is in memory.
    with pytest.raises(BufferError):
        libvalkey.pack_command(("SET", "a", memoryview(b"abcdef")[::2]))


def test_memoryview_of_a_slice():
    assert libvalkey.pack_command(("SET", "a", memoryview(b"abcdef")[1:4])) == (
        b"*3\r\n$3\r\nSET\r\n$1\r\na\r\n$3\r\nbcd\r\n"
    )


def test_int_with_more_digits_than_python_converts():
    limit = sys.get_int_max_str_digits()
    sys.set_int_max_str_digits(640)
    try:
        with pytest.raises(ValueError):
            libvalkey.pack_command(("SET", "a", 10**700))
    finally:
        sys.set_int_max_str_digits(limit)


def test_float_subclass_with_failing_repr():
    class BadFloat(float):
        def __repr__(self):
            raise KeyError("repr")

    with pytest.raises(KeyError):
        libvalkey.pack_command(("SET", "a", BadFloat(1.5)))


def test_str_that_cannot_be_encoded():
    with pytest.raises(UnicodeEncodeError):
        libvalkey.pack_command(("SET", "a", "\ud800"))
