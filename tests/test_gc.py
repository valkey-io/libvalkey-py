import gc
import sys
import weakref

import pytest

import libvalkey

IS_CPYTHON = sys.implementation.name == "cpython"


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


@pytest.mark.skipif(
    not hasattr(sys, "getrefcount"), reason="PyPy has no sys.getrefcount"
)
def test_reader_releases_type_reference():
    # Bind the type to a local name: repeatedly loading libvalkey.Reader lets
    # CPython's LOAD_ATTR cache hold a reference, which skews the count.
    Reader = libvalkey.Reader
    before = sys.getrefcount(Reader)
    for _ in range(1000):
        Reader()
    assert sys.getrefcount(Reader) == before


# PyPy's cpyext keeps the C type mirror of a subclass alive, so the class is
# never collected no matter what Reader_traverse visits.
@pytest.mark.skipif(
    not IS_CPYTHON,
    reason="PyPy keeps cpyext type objects alive, so the subclass never dies",
)
def test_reader_subclass_cycle_is_collected():
    def make_cycle():
        class Local(libvalkey.Reader):
            instance: "Local"

        Local.instance = Local()
        return weakref.ref(Local)

    ref = make_cycle()
    gc.collect()
    assert ref() is None, "Reader subclass cycle was not collected"
