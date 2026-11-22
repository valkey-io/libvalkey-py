"""Tests for use of the extension on free-threaded (PEP 703) CPython builds.

The stress tests are only meaningful when the GIL is actually disabled, but
they check invariants that must also hold with the GIL, so they run on every
interpreter.
"""

import codecs
import gc
import itertools
import os
import platform
import subprocess
import sys
import sysconfig
import threading
import time

import pytest

import libvalkey

FREE_THREADED = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))

# These rely on CPython's reference counting (an object is finalized as soon as its
# last reference is dropped) and on how it looks codecs up.
cpython_only = pytest.mark.skipif(
    platform.python_implementation() != "CPython",
    reason="relies on CPython internals",
)

THREADS = 4
ARRAY_LEN = 16
TIMEOUT = 60  # seconds; the tests take well under one when nothing is wrong

# Every RESP3 type, nested. Parsed as COMPLEX_EXPECTED, with an error for "d".
COMPLEX = (
    b"%5\r\n"
    b"+a\r\n~2\r\n:1\r\n:2\r\n"
    b"+b\r\n*3\r\n,1.5\r\n#t\r\n_\r\n"
    b"+c\r\n=8\r\ntxt:abcd\r\n"
    b"+d\r\n-ERR x\r\n"
    b"+e\r\n>2\r\n+x\r\n$2\r\nyz\r\n"
)


def check_complex(reply):
    assert isinstance(reply, dict) and list(reply) == [b"a", b"b", b"c", b"d", b"e"]
    assert reply[b"a"] == {1, 2}
    assert reply[b"b"] == [1.5, True, None]
    assert reply[b"c"] == b"abcd"
    assert isinstance(reply[b"d"], libvalkey.ReplyError)
    assert reply[b"d"].args == ("ERR x",)
    assert reply[b"e"] == [b"x", b"yz"]


def run_in_threads(*targets):
    """Start every target at once, wait for all of them and re-raise the first error."""
    barrier = threading.Barrier(len(targets), timeout=TIMEOUT)
    errors = []

    def runner(target):
        try:
            barrier.wait()
            target()
        except BaseException as exc:
            errors.append(exc)

    threads = [threading.Thread(target=runner, args=(t,), daemon=True) for t in targets]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(TIMEOUT)
    if errors:
        raise errors[0]
    assert not any(thread.is_alive() for thread in threads), "threads did not finish"


def array_reply(value):
    """RESP array of ARRAY_LEN copies of an integer (long enough to overlap)."""
    return b"*%d\r\n" % ARRAY_LEN + (b":%d\r\n" % value) * ARRAY_LEN


def drain(reader, sink, decode=True):
    while True:
        reply = reader.gets(decode)
        if reply is False:
            return
        sink.append(reply)


@pytest.mark.skipif(not FREE_THREADED, reason="requires a free-threaded build")
def test_import_does_not_enable_gil():
    env = dict(os.environ)
    env.pop("PYTHON_GIL", None)
    package_root = os.path.dirname(os.path.dirname(os.path.abspath(libvalkey.__file__)))
    env["PYTHONPATH"] = os.pathsep.join(
        filter(None, [package_root, env.get("PYTHONPATH")])
    )

    proc = subprocess.run(
        [
            sys.executable,
            "-W",
            "error::RuntimeWarning",
            "-c",
            "import sys, libvalkey; sys.exit(int(sys._is_gil_enabled()))",
        ],
        env=env,
        capture_output=True,
        text=True,
    )

    assert proc.returncode == 0, proc.stderr


def test_independent_readers_in_threads():
    """The usual pattern: every thread (connection) has its own Reader."""
    rounds = 300

    def target():
        for _ in range(rounds):
            reader = libvalkey.Reader(encoding=None)
            reader.feed(COMPLEX)
            check_complex(reader.gets())
            assert reader.gets() is False
            reader.feed(COMPLEX, 0, 10)  # an incomplete reply, dropped with the Reader

    run_in_threads(*[target] * THREADS)


def test_concurrent_feed_and_gets_on_shared_reader():
    """Replies fed from several threads are each delivered exactly once, intact."""
    producers = 2
    per_producer = 2000
    reader = libvalkey.Reader()
    done = threading.Event()
    lock = threading.Lock()
    remaining = [producers]
    sinks = [[] for _ in range(THREADS)]

    def produce(offset):
        def target():
            try:
                for i in range(per_producer):
                    reader.feed(array_reply(offset + i))
            finally:
                # Always signal completion, or a failing producer would leave
                # the consumers spinning forever.
                with lock:
                    remaining[0] -= 1
                    if remaining[0] == 0:
                        done.set()

        return target

    def consume(sink):
        def target():
            while not done.is_set():
                drain(reader, sink)
                time.sleep(0)
            drain(reader, sink)

        return target

    run_in_threads(
        *[produce(p * per_producer) for p in range(producers)],
        *[consume(sink) for sink in sinks],
    )

    replies = [reply for sink in sinks for reply in sink]
    assert len(replies) == producers * per_producer
    for reply in replies:
        assert reply == [reply[0]] * ARRAY_LEN
    assert sorted(reply[0] for reply in replies) == list(
        range(producers * per_producer)
    )


def test_concurrent_gets_with_different_decoding():
    """The decode flag passed to gets() applies to that call only."""
    total = 20000
    reader = libvalkey.Reader(encoding="utf-8")
    reader.feed(b"$5\r\nhello\r\n" * total)
    decoded, raw = [], []

    def consume(sink, decode):
        return lambda: drain(reader, sink, decode)

    run_in_threads(
        consume(decoded, True),
        consume(decoded, True),
        consume(raw, False),
        consume(raw, False),
    )

    assert len(decoded) + len(raw) == total
    assert all(reply == "hello" for reply in decoded)
    assert all(reply == b"hello" for reply in raw)


def test_concurrent_set_encoding_while_decoding():
    """Changing the decode settings never affects a decode that is in progress."""
    # Looking a codec up the first time can import it, which is Python code that
    # the lookup in __init__() would pause for, and a call that is refused then
    # is not what this test is about.
    codecs.lookup("utf-8")
    codecs.lookup("latin-1")
    total = 20000
    reader = libvalkey.Reader(encoding="utf-8")
    reader.feed(b"$5\r\nhello\r\n" * total)
    sinks = [[], []]

    def consume(sink):
        return lambda: drain(reader, sink)

    def reconfigure():
        for i in range(100_000):  # bounded, in case the consumers fail
            if not reader.has_data():
                return
            if i % 2:
                reader.set_encoding("latin-1", "replace")
            else:
                reader.__init__(encoding="utf-8")

    run_in_threads(consume(sinks[0]), consume(sinks[1]), reconfigure, reconfigure)

    replies = sinks[0] + sinks[1]
    assert len(replies) == total
    assert all(reply == "hello" for reply in replies)


def test_concurrent_mixed_operations_on_shared_reader():
    """Every public Reader method can be called concurrently without crashing."""
    codecs.lookup("utf-8")
    reader = libvalkey.Reader()
    iterations = 2000

    def feeder():
        for i in range(iterations):
            reader.feed(array_reply(i))

    def getter():
        for _ in range(iterations):
            reader.gets()

    def inspector():
        for _ in range(iterations):
            assert reader.len() >= 0
            assert reader.has_data() in (True, False)
            assert reader.getmaxbuf() >= 0
            assert reader.convertSetsToLists in (True, False)

    def configurer():
        for i in range(iterations):
            reader.setmaxbuf(None if i % 2 else 1 << 20)
            reader.set_encoding("utf-8" if i % 2 else None)
            reader.__init__(encoding="utf-8" if i % 2 else None, convertSetsToLists=i % 2 == 0)

    run_in_threads(feeder, feeder, getter, getter, inspector, configurer)


def test_concurrent_pack_command():
    cmd = ("SET", "key", 42, b"value", 1.5, memoryview(b"view"))
    expected = libvalkey.pack_command(cmd)

    def target():
        for _ in range(5000):
            assert libvalkey.pack_command(cmd) == expected

    run_in_threads(*[target] * THREADS)


def test_convert_sets_to_lists_can_change_between_gets_calls():
    """Which container a reply is made of is decided when it starts."""
    reader = libvalkey.Reader()
    reader.feed(b"~3\r\n:1\r\n:2\r\n")
    assert reader.gets() is False
    reader.__init__(convertSetsToLists=True)
    reader.feed(b":3\r\n")
    assert reader.gets() == {1, 2, 3}

    reader.__init__(convertSetsToLists=True)
    reader.feed(b"*1\r\n~3\r\n:1\r\n:2\r\n")
    assert reader.gets() is False
    reader.__init__(convertSetsToLists=False)
    reader.feed(b":3\r\n")
    assert reader.gets() == [[1, 2, 3]]


def test_callback_cannot_reenter_the_reader():
    """Python code run by gets() cannot feed, read or re-initialize its Reader."""
    refused = {}
    calls = []

    def reply_error(message):
        calls.append(message)
        if len(calls) == 1:  # a gets() that is let in would call this again
            for name, call in (
                ("feed", lambda: reader.feed(b"+OK\r\n")),
                ("gets", reader.gets),
                ("__init__", reader.__init__),
            ):
                try:
                    call()
                except RuntimeError as exc:
                    refused[name] = str(exc)
        return ValueError(message)

    reader = libvalkey.Reader(replyError=reply_error)
    reader.feed(b"*2\r\n-ERR first\r\n:2\r\n")
    first, second = reader.gets()

    assert sorted(refused) == ["__init__", "feed", "gets"]
    assert all("is still running" in message for message in refused.values())
    assert calls == ["ERR first"]
    # The refused calls neither took the reply nor added to the stream.
    assert isinstance(first, ValueError) and str(first) == "ERR first"
    assert second == 2
    assert not reader.has_data()
    reader.feed(b"+OK\r\n")
    assert reader.gets() == b"OK"


def test_other_threads_cannot_enter_a_reader_while_a_callback_runs():
    """A thread that calls in while gets() waits in Python code gets an error,
    instead of taking the reply that is being built."""
    entered, release = threading.Event(), threading.Event()

    def reply_error(message):
        entered.set()
        release.wait(TIMEOUT)
        return ValueError(message)

    reader = libvalkey.Reader(replyError=reply_error)
    reader.feed(b"*2\r\n-ERR first\r\n:2\r\n")
    result = []

    def getter():
        try:
            result.append(reader.gets())
        except BaseException as exc:
            result.append(exc)

    thread = threading.Thread(target=getter, daemon=True)
    thread.start()
    try:
        assert entered.wait(TIMEOUT)
        for call in (lambda: reader.feed(b"+OK\r\n"), reader.gets, reader.__init__):
            with pytest.raises(RuntimeError, match="is still running"):
                call()
    finally:
        release.set()
        thread.join(TIMEOUT)

    assert not thread.is_alive()
    assert len(result) == 1 and not isinstance(result[0], BaseException), result
    first, second = result[0]
    assert isinstance(first, ValueError) and str(first) == "ERR first"
    assert second == 2
    reader.feed(b"+OK\r\n")
    assert reader.gets() == b"OK"


def test_shared_reader_with_python_callbacks_and_gc():
    """Python code that runs inside gets() lets other threads, and the garbage
    collector, in. Calls that land in that window are refused, and nothing that
    is accepted is lost, repeated or mixed up with another reply."""
    producers, per_producer = 2, 300
    total = producers * per_producer
    # Python code, so that gets() can be interrupted while it builds a reply.
    reader = libvalkey.Reader(replyError=lambda message: ValueError(message))
    lock = threading.Lock()
    remaining = [producers]
    sinks = [[] for _ in range(2)]

    def refused(exc):
        assert isinstance(exc, RuntimeError) and "is still running" in str(exc), exc

    def produce(offset):
        def target():
            try:
                for i in range(per_producer):
                    data = b"*8\r\n" + (b"-ERR %d\r\n" % (offset + i)) * 8
                    while True:
                        try:
                            reader.feed(data)
                            break
                        except RuntimeError as exc:
                            refused(exc)
                            time.sleep(0)
            finally:
                with lock:
                    remaining[0] -= 1

        return target

    def consume(sink):
        def target():
            deadline = time.monotonic() + TIMEOUT
            while (remaining[0] or reader.has_data()) and time.monotonic() < deadline:
                try:
                    reply = reader.gets()
                except RuntimeError as exc:
                    refused(exc)
                    continue
                if reply is False:
                    time.sleep(0)
                else:
                    sink.append(reply)

        return target

    def collect():
        while remaining[0]:
            gc.collect()
            time.sleep(0.001)

    run_in_threads(
        *[produce(p * per_producer) for p in range(producers)],
        *[consume(sink) for sink in sinks],
        collect,
    )

    replies = [reply for sink in sinks for reply in sink]
    assert len(replies) == total
    ids = []
    for reply in replies:
        assert len(reply) == 8 and all(isinstance(e, ValueError) for e in reply)
        assert len({str(e) for e in reply}) == 1
        ids.append(int(str(reply[0]).rsplit(" ", 1)[1]))
    assert sorted(ids) == list(range(total))


def test_reader_is_idle_again_after_gets_fails():
    """Whichever way gets() fails, the next call is not refused as re-entrant."""

    def reply_error(message):
        raise KeyError(message)

    reader = libvalkey.Reader(replyError=reply_error, encoding="utf-8")
    with pytest.raises(TypeError):
        reader.gets("not a flag")
    reader.feed(b"-ERR\r\n")
    with pytest.raises(KeyError):
        reader.gets()
    reader.feed(b"+\xff\r\n")
    with pytest.raises(UnicodeDecodeError):
        reader.gets()
    reader.feed(b"!\r\n")
    for _ in range(2):  # a protocol error is permanent, but it is still that error
        with pytest.raises(libvalkey.ProtocolError):
            reader.gets()


@cpython_only
def test_reinitialize_installs_the_new_callable_before_releasing_the_old():
    """Releasing the old callable runs its __del__, which can use the Reader."""
    seen = []

    class OldReplyError:
        def __call__(self, message):
            return ValueError("old " + message)

        def __del__(self):
            reader.feed(b"-ERR\r\n")
            seen.append(str(reader.gets()))

    reader = libvalkey.Reader(replyError=OldReplyError())
    reader.__init__(replyError=lambda message: ValueError("new " + message))
    assert seen == ["new ERR"]


@cpython_only
def test_reinitialize_installs_the_new_sentinel_before_releasing_the_old():
    seen = []

    class OldSentinel:
        def __del__(self):
            seen.append(reader.gets())

    sentinel = object()
    reader = libvalkey.Reader(notEnoughData=OldSentinel())
    reader.__init__(notEnoughData=sentinel)
    assert len(seen) == 1 and seen[0] is sentinel


def reinitialize_in_two_threads(old, first, second):
    """Re-initialize a Reader in one thread, which pauses in the __del__ of the
    object `old` (a Reader argument) held, and then in another; returns the Reader."""
    created, entered, release = (threading.Event() for _ in range(3))
    readers, errors = [], []

    class Blocking:
        def __call__(self, message):
            return ValueError(message)

        def __del__(self):
            entered.set()
            release.wait(TIMEOUT)

    def run_first():
        # Created here so that this thread owns the object and finalizes it.
        try:
            readers.append(libvalkey.Reader(**{old: Blocking()}))
            created.set()
            readers[0].__init__(**first)
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(target=run_first, daemon=True)
    thread.start()
    try:
        assert created.wait(TIMEOUT) and entered.wait(TIMEOUT)
        readers[0].__init__(**second)
    finally:
        release.set()
        thread.join(TIMEOUT)

    assert not thread.is_alive() and not errors, errors
    return readers[0]


@cpython_only
def test_reinitialize_while_a_finalizer_of_the_old_callable_blocks():
    """Another thread can re-initialize a Reader whose __init__() is paused in the
    __del__ of the callable it replaced."""
    reader = reinitialize_in_two_threads(
        "replyError",
        {"replyError": ConnectionError},
        {"replyError": KeyError},
    )
    reader.feed(b"-ERR\r\n")
    assert type(reader.gets()) is KeyError


@cpython_only
def test_concurrent_reinitialize_leaves_one_whole_configuration():
    """A paused __init__() cannot overwrite part of one that completed meanwhile."""
    reader = reinitialize_in_two_threads(
        "protocolError",
        {"protocolError": OSError, "replyError": IndexError, "encoding": "latin-1"},
        {"protocolError": ValueError, "replyError": KeyError, "encoding": "utf-8"},
    )
    reader.feed(b"-ERR\r\n$2\r\n\xc3\xa9\r\n!\r\n")
    assert type(reader.gets()) is KeyError
    assert reader.gets() == "\xe9"
    with pytest.raises(ValueError):
        reader.gets()


slow_codecs = itertools.count()


@cpython_only
def test_reinitialize_is_refused_if_gets_starts_while_it_looks_up_a_codec():
    """Looking a codec up runs Python code, so a gets() can start in the middle of
    __init__(); the callables it is calling must not be replaced under it."""
    name = "libvalkey_test_slow_codec_%d" % next(slow_codecs)  # new: lookups are cached
    in_lookup, proceed, init_done = (threading.Event() for _ in range(3))
    init_errors = []

    def search(requested):
        if requested != name:
            return None
        in_lookup.set()
        proceed.wait(TIMEOUT)
        return codecs.lookup("utf-8")

    codecs.register(search)

    def reply_error(message):
        proceed.set()  # lets __init__() finish its lookup, now that gets() is running
        init_done.wait(TIMEOUT)
        return ValueError(message)

    reader = libvalkey.Reader(replyError=reply_error)

    def reinitialize():
        try:
            reader.__init__(replyError=ConnectionError, encoding=name)
        except BaseException as exc:
            init_errors.append(exc)
        init_done.set()

    thread = threading.Thread(target=reinitialize, daemon=True)
    thread.start()
    try:
        assert in_lookup.wait(TIMEOUT)
        reader.feed(b"-ERR\r\n")
        reply = reader.gets()
    finally:
        proceed.set()
        init_done.wait(TIMEOUT)
        thread.join(TIMEOUT)
        codecs.unregister(search)

    assert not thread.is_alive()
    assert type(reply) is ValueError and str(reply) == "ERR"
    assert len(init_errors) == 1 and isinstance(init_errors[0], RuntimeError)
    assert "__init__" in str(init_errors[0])
