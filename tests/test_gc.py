import gc
import sys
import weakref

import libvalkey


def test_reader_gc():
    class A:
        def __init__(self):
            self.reader = libvalkey.Reader(replyError=self.reply_error)

        def reply_error(self, error):
            return Exception()

    A()
    gc.collect()

    assert not any(
        isinstance(o, A) for o in gc.get_objects()
    ), "Referent was not collected"


def test_reader_releases_type_reference():
    # Bind the type to a local name: repeatedly loading libvalkey.Reader lets
    # CPython's LOAD_ATTR cache hold a reference, which skews the count.
    Reader = libvalkey.Reader
    before = sys.getrefcount(Reader)
    for _ in range(1000):
        Reader()
    assert sys.getrefcount(Reader) == before


def test_reader_subclass_cycle_is_collected():
    def make_cycle():
        class Local(libvalkey.Reader):
            instance: "Local"

        Local.instance = Local()
        return weakref.ref(Local)

    ref = make_cycle()
    gc.collect()
    assert ref() is None, "Reader subclass cycle was not collected"
