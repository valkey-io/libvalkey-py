# libvalkey-py

[![Build Status](https://github.com/valkey-io/libvalkey-py/actions/workflows/integration.yaml/badge.svg)](https://github.com/valkey-io/libvalkey-py/actions/workflows/integration.yaml)
[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)
[![pypi](https://badge.fury.io/py/libvalkey.svg)](https://pypi.org/project/libvalkey/)

Python extension that wraps protocol parsing code in [libvalkey][libvalkey].
It primarily speeds up parsing of multi bulk replies.

[libvalkey]: http://github.com/valkey-io/libvalkey

## Install

libvalkey-py is available on [PyPI](https://pypi.org/project/libvalkey/), and can be installed via:

```bash
pip install libvalkey
```
## Building and Testing

Building this repository requires a recursive checkout of submodules, and building libvalkey. The following example shows how to clone, compile, and run tests. Please note - you will need the gcc installed.

```bash
git clone --recurse-submodules https://github.com/valkey-io/libvalkey-py
python setup.py build_ext --inplace
python -m pytest
```

### Requirements

libvalkey-py requires **Python 3.8+**.

Make sure Python development headers are available when installing libvalkey-py.
On Ubuntu/Debian systems, install them with `apt-get install python3-dev`.

## Usage

The `libvalkey` module contains the `Reader` class. This class is responsible for
parsing replies from the stream of data that is read from a Redis connection.
It does not contain functionality to handle I/O.

### Reply parser

The `Reader` class has two methods that are used when parsing replies from a
stream of data. `Reader.feed` takes a string argument that is appended to the
internal buffer. `Reader.gets` reads this buffer and returns a reply when the
buffer contains a full reply. If a single call to `feed` contains multiple
replies, `gets` should be called multiple times to extract all replies.

Example:

```python
>>> reader = libvalkey.Reader()
>>> reader.feed("$5\r\nhello\r\n")
>>> reader.gets()
b'hello'
```

When the buffer does not contain a full reply, `gets` returns `False`.
This means extra data is needed and `feed` should be called again before calling
`gets` again. Alternatively you could provide custom sentinel object via parameter,
which is useful for RESP3 protocol where native boolean types are supported:

Example:

```python
>>> reader.feed("*2\r\n$5\r\nhello\r\n")
>>> reader.gets()
False
>>> reader.feed("$5\r\nworld\r\n")
>>> reader.gets()
[b'hello', b'world']
>>> reader = libvalkey.Reader(notEnoughData=Ellipsis)
>>> reader.gets()
Ellipsis
```

#### Unicode

`libvalkey.Reader` is able to decode bulk data to any encoding Python supports.
To do so, specify the encoding you want to use for decoding replies when
initializing it:

```python
>>> reader = libvalkey.Reader(encoding="utf-8", errors="strict")
>>> reader.feed(b"$3\r\n\xe2\x98\x83\r\n")
>>> reader.gets()
'☃'
```

Decoding of bulk data will be attempted using the specified encoding and
error handler. If the error handler is `'strict'` (the default), a
`UnicodeDecodeError` is raised when data cannot be dedcoded. This is identical
to Python's default behavior. Other valid values to `errors` include
`'replace'`, `'ignore'`, and `'backslashreplace'`. More information on the
behavior of these error handlers can be found
[here](https://docs.python.org/3/howto/unicode.html#the-string-type).


When the specified encoding cannot be found, a `LookupError` will be raised
when calling `gets` for the first reply with bulk data.

#### Error handling

When a protocol error occurs (because of multiple threads using the same
socket, or some other condition that causes a corrupt stream), the error
`libvalkey.ProtocolError` is raised. Because the buffer is read in a lazy
fashion, it will only be raised when `gets` is called and the first reply in
the buffer contains an error. There is no way to recover from a faulty protocol
state, so when this happens, the I/O code feeding data to `Reader` should
probably reconnect.

The server can reply with error replies (`-ERR ...`). For these replies, the
custom error class `libvalkey.ReplyError` is returned, **but not raised**.

When other error types should be used (so existing code doesn't have to change
its `except` clauses), `Reader` can be initialized with the `protocolError` and
`replyError` keywords. These keywords should contain a *class* that is a
subclass of `Exception`. When not provided, `Reader` will use the default
error types.

## Benchmarks

The repository contains a benchmarking script in the `benchmark` directory,
which uses [gevent](http://gevent.org/) to have non-blocking I/O and valkey-py
to handle connections. These benchmarks are done with a patched version of
valkey-py that uses libvalkey-py when it is available.

All benchmarks are done with 10 concurrent connections.

* SET key value + GET key
  * valkey-py: 11.76 Kops
  * valkey-py *with* libvalkey-py: 13.40 Kops
  * improvement: **1.1x**

List entries in the following tests are 5 bytes.

* LRANGE list 0 **9**:
  * valkey-py: 4.78 Kops
  * valkey-py *with* libvalkey-py: 12.94 Kops
  * improvement: **2.7x**
* LRANGE list 0 **99**:
  * valkey-py: 0.73 Kops
  * valkey-py *with* libvalkey-py: 11.90 Kops
  * improvement: **16.3x**
* LRANGE list 0 **999**:
  * valkey-py: 0.07 Kops
  * valkey-py *with* libvalkey-py: 5.83 Kops
  * improvement: **83.2x**

Throughput improvement for simple SET/GET is minimal, but the larger multi bulk replies
get, the larger the performance improvement is.

## License

This code is released under the BSD license, as the license of hiredis-py at
the time of fork.
