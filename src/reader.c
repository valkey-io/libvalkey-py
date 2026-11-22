#include "reader.h"
#include "libvalkey.h"

#include <assert.h>

static void Reader_dealloc(libvalkey_ReaderObject *self);
static int Reader_traverse(libvalkey_ReaderObject *self, visitproc visit, void *arg);
static int Reader_init(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds);
static PyObject *Reader_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static PyObject *Reader_feed(libvalkey_ReaderObject *self, PyObject *args);
static PyObject *Reader_gets(libvalkey_ReaderObject *self, PyObject *args);
static PyObject *Reader_setmaxbuf(libvalkey_ReaderObject *self, PyObject *arg);
static PyObject *Reader_getmaxbuf(libvalkey_ReaderObject *self);
static PyObject *Reader_len(libvalkey_ReaderObject *self);
static PyObject *Reader_has_data(libvalkey_ReaderObject *self);
static PyObject *Reader_set_encoding(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds);
static PyObject *Reader_convertSetsToLists(PyObject *self, void *closure);

static PyMethodDef libvalkey_ReaderMethods[] = {
    {"feed", (PyCFunction)Reader_feed, METH_VARARGS, NULL },
    {"gets", (PyCFunction)Reader_gets, METH_VARARGS, NULL },
    {"setmaxbuf", (PyCFunction)Reader_setmaxbuf, METH_O, NULL },
    {"getmaxbuf", (PyCFunction)Reader_getmaxbuf, METH_NOARGS, NULL },
    {"len", (PyCFunction)Reader_len, METH_NOARGS, NULL },
    {"has_data", (PyCFunction)Reader_has_data, METH_NOARGS, NULL },
    {"set_encoding", (PyCFunction)Reader_set_encoding, METH_VARARGS | METH_KEYWORDS, NULL },
    { NULL }  /* Sentinel */
};

static PyGetSetDef libvalkey_ReaderGetSet[] = {
    {"convertSetsToLists", (getter)Reader_convertSetsToLists, NULL, NULL, NULL},
    {NULL}  /* Sentinel */
};

PyTypeObject libvalkey_ReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    MOD_LIBVALKEY ".Reader",        /*tp_name*/
    sizeof(libvalkey_ReaderObject), /*tp_basicsize*/
    0,                            /*tp_itemsize*/
    (destructor)Reader_dealloc,   /*tp_dealloc*/
    0,                            /*tp_print*/
    0,                            /*tp_getattr*/
    0,                            /*tp_setattr*/
    0,                            /*tp_compare*/
    0,                            /*tp_repr*/
    0,                            /*tp_as_number*/
    0,                            /*tp_as_sequence*/
    0,                            /*tp_as_mapping*/
    0,                            /*tp_hash */
    0,                            /*tp_call*/
    0,                            /*tp_str*/
    0,                            /*tp_getattro*/
    0,                            /*tp_setattro*/
    0,                            /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    "Valkey protocol reader",    /*tp_doc */
    (traverseproc)Reader_traverse,/*tp_traverse */
    0,                            /*tp_clear */
    0,                            /*tp_richcompare */
    0,                            /*tp_weaklistoffset */
    0,                            /*tp_iter */
    0,                            /*tp_iternext */
    libvalkey_ReaderMethods,        /*tp_methods */
    0,                            /*tp_members */
    libvalkey_ReaderGetSet,        /*tp_getset */
    0,                            /*tp_base */
    0,                            /*tp_dict */
    0,                            /*tp_descr_get */
    0,                            /*tp_descr_set */
    0,                            /*tp_dictoffset */
    (initproc)Reader_init,        /*tp_init */
    0,                            /*tp_alloc */
    Reader_new,                   /*tp_new */
};

/* Keeps the first exception raised while a reply is built, to raise it once the
 * reply is complete (raising it right away would leave the Reader in an
 * inconsistent state). Later ones are dropped. Either way the exception
 * indicator ends up clear, so that parsing can go on. */
static void stashError(libvalkey_ReaderObject *self) {
    if (self->error.ptype == NULL)
        PyErr_Fetch(&(self->error.ptype), &(self->error.pvalue),
                &(self->error.ptraceback));
    else
        PyErr_Clear();
}

/* Stores obj as item idx of the list that is being built. Takes the reference
 * to obj whether it succeeds or not, like PyList_SetItem(). The list is not
 * visible to other threads yet, so it does not need its lock. */
static int storeListItem(PyObject *list, int idx, PyObject *obj) {
#ifdef PYPY_VERSION
    return PyList_SetItem(list, idx, obj);
#else
    if (!PyList_CheckExact(list) || idx < 0 || idx >= PyList_GET_SIZE(list)) {
        Py_DECREF(obj);
        PyErr_SetString(PyExc_SystemError, "unexpected position in a reply");
        return -1;
    }
    PyList_SET_ITEM(list, idx, obj);
    return 0;
#endif
}

/* Takes the reference to obj, which is NULL if its creation failed. Returns obj
 * on success; the parent holds the reference then. Returns NULL on failure,
 * which libvalkey reports as an error. */
static void *tryParentize(const valkeyReadTask *task, PyObject *obj) {
    libvalkey_ReaderObject *self = (libvalkey_ReaderObject*)task->privdata;
    PyObject *parent;

    if (obj == NULL) {
        /* Creating it failed, with an exception set unless it ran out of memory. */
        return NULL;
    }
    if (task->parent == NULL)
        return obj;

    parent = (PyObject*)task->parent->obj;
    switch (task->parent->type) {
        case VALKEY_REPLY_MAP:
            if (task->idx % 2 == 0) {
                /* Save the object as a key. */
                PyObject *stale = self->pendingObject;
                self->pendingObject = obj;
                Py_XDECREF(stale);
            } else {
                PyObject *key = self->pendingObject;
                int x;
                if (key == NULL) {
                    Py_DECREF(obj);
                    return NULL;
                }
                self->pendingObject = NULL;
                x = PyDict_SetItem(parent, key, obj);
                Py_DECREF(obj);
                Py_DECREF(key);
                if (x < 0) {
                    return NULL;
                }
            }
            break;
        case VALKEY_REPLY_SET:
            /* Go by what the parent is rather than by convertSetsToLists: that
             * can change between two calls to gets(), in the middle of a reply. */
            if (PySet_CheckExact(parent)) {
                int x = PySet_Add(parent, obj);
                Py_DECREF(obj);
                if (x < 0) {
                    return NULL;
                }
                break;
            }
            /* fallthrough */
        default:
            if (storeListItem(parent, task->idx, obj) < 0) {
                return NULL;
            }
    }
    return obj;
}

static PyObject *createDecodedString(libvalkey_ReaderObject *self, const char *str, size_t len) {
    PyObject *obj;

    if (self->encoding == NULL || !self->shouldDecode) {
        obj = PyBytes_FromStringAndSize(str, len);
    } else {
        /* Decoding can run Python code (a codec or an error handler) that
         * replaces this Reader's encoding, so hold references to the names. */
        PyObject *encoding = self->encoding;
        PyObject *errors = self->errors;

        Py_INCREF(encoding);
        Py_XINCREF(errors);
        obj = PyUnicode_Decode(str, len, PyBytes_AS_STRING(encoding),
                               errors ? PyBytes_AS_STRING(errors) : NULL);
        Py_DECREF(encoding);
        Py_XDECREF(errors);
        if (obj == NULL) {
            stashError(self);

            /* Return Py_None as placeholder to let the error bubble up and
             * be used when a full reply in Reader#gets(). */
            obj = Py_None;
            Py_INCREF(obj);
        }
    }

    return obj;
}

static void *createError(PyObject *errorCallable, char *errstr, size_t len) {
    PyObject *obj, *errmsg;

    errmsg = PyUnicode_DecodeUTF8(errstr, len, "replace");
    if (errmsg == NULL)
        return NULL;

    obj = PyObject_CallFunctionObjArgs(errorCallable, errmsg, NULL);
    Py_DECREF(errmsg);
    /* obj can be NULL if custom error class raised another exception */

    return obj;
}

static void *createStringObject(const valkeyReadTask *task, char *str, size_t len) {
    libvalkey_ReaderObject *self = (libvalkey_ReaderObject*)task->privdata;
    PyObject *obj;

    if (task->type == VALKEY_REPLY_ERROR) {
        obj = createError(self->replyErrorClass, str, len);
        if (obj == NULL) {
            stashError(self);
            obj = Py_None;
            Py_INCREF(obj);
        }
    } else {
        if (task->type == VALKEY_REPLY_VERB) {
            /* Skip 4 bytes of verbatim type header. libvalkey has checked that
             * they are there. */
            size_t skip = len < 4 ? len : 4;
            str += skip;
            len -= skip;
        }
        obj = createDecodedString(self, str, len);
    }
    return tryParentize(task, obj);
}

static void *createArrayObject(const valkeyReadTask *task, size_t elements) {
    libvalkey_ReaderObject *self = (libvalkey_ReaderObject*)task->privdata;
    PyObject *obj;
    switch (task->type) {
        case VALKEY_REPLY_MAP:
            obj = PyDict_New();
            break;
        case VALKEY_REPLY_SET:
            if (!self->convertSetsToLists) {
                obj = PySet_New(NULL);
                break;
            }
            /* fallthrough */
        default:
            obj = PyList_New(elements);
    }
    if (obj != NULL && task->parent != NULL &&
            task->parent->type == VALKEY_REPLY_MAP && task->idx % 2 == 0) {
        /* A dict, set or list is not hashable, so it cannot be a key. Fail now:
         * the key would stay pending while its own entries are read, and they
         * would need the same slot. */
        PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'",
                     Py_TYPE(obj)->tp_name);
        Py_DECREF(obj);
        return NULL;
    }
    return tryParentize(task, obj);
}

static void *createIntegerObject(const valkeyReadTask *task, long long value) {
    PyObject *obj;
    obj = PyLong_FromLongLong(value);
    return tryParentize(task, obj);
}

static void *createDoubleObject(const valkeyReadTask *task, double value, char *str, size_t le) {
    PyObject *obj;
    obj = PyFloat_FromDouble(value);
    return tryParentize(task, obj);
}

static void *createNilObject(const valkeyReadTask *task) {
    PyObject *obj = Py_None;
    Py_INCREF(obj);
    return tryParentize(task, obj);
}

static void *createBoolObject(const valkeyReadTask *task, int bval) {
    PyObject *obj;
    obj = PyBool_FromLong((long)bval);
    return tryParentize(task, obj);
}

static void freeObject(void *obj) {
    Py_XDECREF(obj);
}

valkeyReplyObjectFunctions libvalkey_ObjectFunctions = {
    createStringObject,  // void *(*createString)(const valkeyReadTask*, char*, size_t);
    createArrayObject,   // void *(*createArray)(const valkeyReadTask*, size_t);
    createIntegerObject, // void *(*createInteger)(const valkeyReadTask*, long long);
    createDoubleObject,  // void *(*createDoubleObject)(const valkeyReadTask*, double, char*, size_t);
    createNilObject,     // void *(*createNil)(const valkeyReadTask*);
    createBoolObject,    // void *(*createBoolObject)(const valkeyReadTask*, int);
    freeObject           // void (*freeObject)(void*);
};

static void Reader_dealloc(libvalkey_ReaderObject *self) {
    PyObject_GC_UnTrack(self);
    Py_CLEAR(self->encoding);
    Py_CLEAR(self->errors);
    /* Releases the reply that was being built, if any. */
    valkeyReaderFree(self->reader);
    self->reader = NULL;
    Py_CLEAR(self->pendingObject);
    Py_CLEAR(self->error.ptype);
    Py_CLEAR(self->error.pvalue);
    Py_CLEAR(self->error.ptraceback);
    Py_CLEAR(self->protocolErrorClass);
    Py_CLEAR(self->replyErrorClass);
    Py_CLEAR(self->notEnoughDataObject);

    ((PyObject *)self)->ob_type->tp_free((PyObject*)self);
}

static int Reader_traverse(libvalkey_ReaderObject *self, visitproc visit, void *arg) {
    Py_VISIT(self->protocolErrorClass);
    Py_VISIT(self->replyErrorClass);
    Py_VISIT(self->notEnoughDataObject);
    Py_VISIT(self->pendingObject);
    Py_VISIT(self->error.ptype);
    Py_VISIT(self->error.pvalue);
    Py_VISIT(self->error.ptraceback);
    if (self->reader != NULL)
        Py_VISIT((PyObject *)self->reader->reply);
    return 0;
}

static int _Reader_check_callable(PyObject *value) {
    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Expected a callable");
        return 0;
    }
    return 1;
}

/* Checks that the codec and the error handler exist (LookupError if not) and
 * copies the names into new bytes objects, NULL for a NULL name. The "z" format
 * of PyArg_Parse*() hands out a pointer into the argument object, which can be
 * freed as soon as the call returns, so the Reader must not keep that pointer.
 * Looking a codec up can run Python code, so this does not change the Reader:
 * what it prepares is installed afterwards, in one step. */
static int _Reader_prepare_names(const char *encoding, const char *errors,
                                 PyObject **encodingName, PyObject **errorsName) {
    PyObject *codecs, *result;
    PyObject *encodingCopy = NULL, *errorsCopy = NULL;

    if (encoding) {  // validate that the encoding exists, raises LookupError if not
        codecs = PyImport_ImportModule("codecs");
        if (!codecs)
            return -1;
        result = PyObject_CallMethod(codecs, "lookup", "s", encoding);
        Py_DECREF(codecs);
        if (!result)
            return -1;
        Py_DECREF(result);
    }
    if (errors) {   // validate that the error handler exists, raises LookupError if not
        codecs = PyImport_ImportModule("codecs");
        if (!codecs)
            return -1;
        result = PyObject_CallMethod(codecs, "lookup_error", "s", errors);
        Py_DECREF(codecs);
        if (!result)
            return -1;
        Py_DECREF(result);
    }

    if (encoding) {
        encodingCopy = PyBytes_FromStringAndSize(encoding, strlen(encoding));
        if (!encodingCopy)
            return -1;
    }
    // NULL errors means "strict", which is what PyUnicode_Decode() assumes
    if (errors) {
        errorsCopy = PyBytes_FromStringAndSize(errors, strlen(errors));
        if (!errorsCopy) {
            Py_XDECREF(encodingCopy);
            return -1;
        }
    }
    *encodingName = encodingCopy;
    *errorsName = errorsCopy;
    return 0;
}

/* Replaces the encoding and errors names, both or neither. They are bytes
 * objects that are replaced and never modified, so a decode that holds its own
 * references to them is not affected. */
static int _Reader_set_encoding(libvalkey_ReaderObject *self, const char *encoding, const char *errors) {
    PyObject *encodingName, *errorsName, *oldEncoding, *oldErrors;

    if (_Reader_prepare_names(encoding, errors, &encodingName, &errorsName) == -1)
        return -1;
    oldEncoding = self->encoding;
    oldErrors = self->errors;
    self->encoding = encodingName;
    self->errors = errorsName;
    Py_XDECREF(oldEncoding);
    Py_XDECREF(oldErrors);
    return 0;
}

/* Thread safety on free-threaded builds
 *
 * A Reader holds mutable state (the libvalkey buffer and parse stack, the decode
 * settings and a stashed error) that must not be touched by two threads at
 * once. Every method that reads or writes it runs inside a critical section on
 * the Reader (Py_BEGIN_CRITICAL_SECTION), which makes each call atomic with
 * respect to the others. With a GIL these are no-ops.
 *
 * A critical section is released while its thread is paused: when it blocks, or
 * when the interpreter stops the world (a GC run started by any thread). That
 * can happen while Python code runs inside gets(): a protocolError/replyError
 * callable, a custom codec or an error handler. Another thread could then enter
 * the parser in the middle of a reply and take or corrupt it, as it can with a
 * GIL when the interpreter switches threads. The parser cannot be re-entered:
 * gets() would take the reply being built, feed() can move the buffer that a
 * decode is reading, and __init__() replaces the callables being called. So
 * gets() marks the Reader busy while it runs, and feed(), gets() and __init__()
 * raise RuntimeError if they are called in the meantime, from the callback
 * itself or from another thread. The other methods only read or replace single
 * fields, and a decode in progress holds its own references to the encoding and
 * errors names, so they stay safe.
 *
 * __init__() and set_encoding() can pause too: looking up a codec can run Python
 * code, and so can releasing an object they replace (a __del__ method). They
 * therefore prepare everything first, install it all in one step and only then
 * release what was replaced, so that a paused call leaves nothing half applied
 * and Python code or another thread finds the Reader complete.
 *
 * The same goes for the objects a reply is made of. The parent of an item is
 * found from the object itself, not from convertSetsToLists, which __init__()
 * may change in between two calls to gets(), and the key of a map entry is held
 * by the Reader until its value arrives and released with the Reader if it
 * never does. */

/* Raises RuntimeError if a gets() call is still running on this Reader. */
static int Reader_check_idle(libvalkey_ReaderObject *self, const char *method) {
    if (self->busy) {
        PyErr_Format(PyExc_RuntimeError,
                     "Reader.%s() called while Reader.gets() is still running "
                     "on this Reader, from a callback or another thread",
                     method);
        return -1;
    }
    return 0;
}

static int Reader_init_impl(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {
        "protocolError",
        "replyError",
        "encoding",
        "errors",
        "notEnoughData",
        "convertSetsToLists",
        NULL,
    };
    PyObject *protocolErrorClass = NULL;
    PyObject *replyErrorClass = NULL;
    PyObject *notEnoughData = NULL;
    char *encoding = NULL;
    char *errors = NULL;
    int convertSetsToLists = 0;
    PyObject *encodingName, *errorsName;
    PyObject *oldProtocolError = NULL, *oldReplyError = NULL, *oldNotEnoughData = NULL;
    PyObject *oldEncoding, *oldErrors;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOzzOp", kwlist,
        &protocolErrorClass, &replyErrorClass, &encoding, &errors, &notEnoughData, &convertSetsToLists))
            return -1;

    /* Check and prepare all of it before changing anything: a call applies all
     * of its arguments or none of them. */
    if (protocolErrorClass && !_Reader_check_callable(protocolErrorClass))
        return -1;
    if (replyErrorClass && !_Reader_check_callable(replyErrorClass))
        return -1;
    if (_Reader_prepare_names(encoding, errors, &encodingName, &errorsName) == -1)
        return -1;

    /* Preparing the names can run Python code, during which another thread can
     * start gets(). Its callables must not be replaced while it runs. */
    if (Reader_check_idle(self, "__init__") == -1) {
        Py_XDECREF(encodingName);
        Py_XDECREF(errorsName);
        return -1;
    }

    /* Install all of it first and release what it replaces afterwards.
     * Releasing an old object can run Python code (a __del__ method) and pause
     * this thread, and by then the Reader must hold the whole new configuration:
     * another thread's __init__() can complete in the meantime, and the rest of
     * this one must not overwrite it. */
    if (protocolErrorClass) {
        oldProtocolError = self->protocolErrorClass;
        Py_INCREF(protocolErrorClass);
        self->protocolErrorClass = protocolErrorClass;
    }
    if (replyErrorClass) {
        oldReplyError = self->replyErrorClass;
        Py_INCREF(replyErrorClass);
        self->replyErrorClass = replyErrorClass;
    }
    if (notEnoughData) {
        oldNotEnoughData = self->notEnoughDataObject;
        Py_INCREF(notEnoughData);
        self->notEnoughDataObject = notEnoughData;
    }
    self->convertSetsToLists = convertSetsToLists;
    oldEncoding = self->encoding;
    oldErrors = self->errors;
    self->encoding = encodingName;
    self->errors = errorsName;

    Py_XDECREF(oldProtocolError);
    Py_XDECREF(oldReplyError);
    Py_XDECREF(oldNotEnoughData);
    Py_XDECREF(oldEncoding);
    Py_XDECREF(oldErrors);
    return 0;
}

static int Reader_init(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds) {
    int res;
    Py_BEGIN_CRITICAL_SECTION(self);
    res = Reader_init_impl(self, args, kwds);
    Py_END_CRITICAL_SECTION();
    return res;
}

static PyObject *Reader_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    libvalkey_ReaderObject *self;
    self = (libvalkey_ReaderObject*)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->reader = valkeyReaderCreateWithFunctions(NULL);
        if (self->reader == NULL) {
            Py_DECREF(self);
            return PyErr_NoMemory();
        }
        self->reader->fn = &libvalkey_ObjectFunctions;
        self->reader->privdata = self;

        self->encoding = NULL;
        self->errors = NULL;  // NULL means "strict" to PyUnicode_Decode()
        self->notEnoughDataObject = Py_False;
        self->shouldDecode = 1;
        self->busy = 0;
        self->protocolErrorClass = LIBVALKEY_STATE->VkErr_ProtocolError;
        self->replyErrorClass = LIBVALKEY_STATE->VkErr_ReplyError;
        self->pendingObject = NULL;
        self->convertSetsToLists = 0;
        Py_INCREF(self->protocolErrorClass);
        Py_INCREF(self->replyErrorClass);
        Py_INCREF(self->notEnoughDataObject);

        self->error.ptype = NULL;
        self->error.pvalue = NULL;
        self->error.ptraceback = NULL;
    }
    return (PyObject*)self;
}

static PyObject *Reader_feed(libvalkey_ReaderObject *self, PyObject *args) {
    Py_buffer buf;
    Py_ssize_t off = 0;
    Py_ssize_t len = -1;
    int refused;

    if (!PyArg_ParseTuple(args, "s*|nn", &buf, &off, &len)) {
        return NULL;
    }

    /* Compare without adding or subtracting values that can be anything the
     * caller passed, which could overflow. */
    if (off < 0 || (len < 0 && len != -1)) {
      PyErr_SetString(PyExc_ValueError, "negative input");
      goto error;
    }

    if (len == -1) {
      if (off > buf.len) {
        PyErr_SetString(PyExc_ValueError, "negative input");
        goto error;
      }
      len = buf.len - off;
    } else if (off > buf.len || len > buf.len - off) {
      PyErr_SetString(PyExc_ValueError, "input is larger than buffer size");
      goto error;
    }

    Py_BEGIN_CRITICAL_SECTION(self);
    refused = Reader_check_idle(self, "feed");
    if (!refused)
        valkeyReaderFeed(self->reader, (char *)buf.buf + off, len);
    Py_END_CRITICAL_SECTION();
    PyBuffer_Release(&buf);
    if (refused)
        return NULL;
    Py_RETURN_NONE;

error:
    PyBuffer_Release(&buf);
    return NULL;
}

static PyObject *Reader_gets_impl(libvalkey_ReaderObject *self) {
    void *reply;
    PyObject *obj;

    if (valkeyReaderGetReply(self->reader, &reply) == VALKEY_ERR) {
        PyObject *err = NULL;
        char *errstr = valkeyReaderGetError(self->reader);
        /* This is a hack to avoid
         * "SystemError: class returned a result with an exception set".
         * It is caused by the fact that one of callbacks already set an
         * exception (i.e. failed PyDict_SetItem). Calling createError()
         * after an exception is set will raise SystemError as per
         * https://github.com/python/cpython/issues/67759.
         * Hence, only call createError() if there is no exception set.
         */
        if (PyErr_Occurred() == NULL) {
            /* protocolErrorClass might be a callable. call it, then use it's type */
            err = createError(self->protocolErrorClass, errstr, strlen(errstr));
        }
        if (err != NULL) {
            obj = PyObject_Type(err);
            PyErr_SetString(obj, errstr);
            Py_DECREF(obj);
            Py_DECREF(err);
        }
        return NULL;
    }

    obj = (PyObject *)reply;
    if (obj == NULL) {
        Py_INCREF(self->notEnoughDataObject);
        return self->notEnoughDataObject;
    } else {
        /* Restore error when there is one. */
        if (self->error.ptype != NULL) {
            PyObject *ptype = self->error.ptype;
            PyObject *pvalue = self->error.pvalue;
            PyObject *ptraceback = self->error.ptraceback;

            self->error.ptype = NULL;
            self->error.pvalue = NULL;
            self->error.ptraceback = NULL;
            /* Releasing the reply can run Python code, so do it while the
             * stash is empty and before the error is set. */
            Py_DECREF(obj);
            PyErr_Restore(ptype, pvalue, ptraceback);
            return NULL;
        }
        return obj;
    }
}

static PyObject *Reader_gets(libvalkey_ReaderObject *self, PyObject *args) {
    int shouldDecode = 1;
    PyObject *res = NULL;

    if (!PyArg_ParseTuple(args, "|i", &shouldDecode)) {
        return NULL;
    }

    Py_BEGIN_CRITICAL_SECTION(self);
    if (Reader_check_idle(self, "gets") == 0) {
        self->busy = 1;
        self->shouldDecode = shouldDecode;
        res = Reader_gets_impl(self);
        self->busy = 0;
    }
    Py_END_CRITICAL_SECTION();
    return res;
}

static PyObject *Reader_setmaxbuf(libvalkey_ReaderObject *self, PyObject *arg) {
    long maxbuf;

    if (arg == Py_None)
        maxbuf = VALKEY_READER_MAX_BUF;
    else {
        maxbuf = PyLong_AsLong(arg);
        if (maxbuf < 0) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_ValueError,
                                "maxbuf value out of range");
            return NULL;
        }
    }
    Py_BEGIN_CRITICAL_SECTION(self);
    self->reader->maxbuf = maxbuf;
    Py_END_CRITICAL_SECTION();

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *Reader_getmaxbuf(libvalkey_ReaderObject *self) {
    size_t maxbuf;
    Py_BEGIN_CRITICAL_SECTION(self);
    maxbuf = self->reader->maxbuf;
    Py_END_CRITICAL_SECTION();
    return PyLong_FromSize_t(maxbuf);
}

static PyObject *Reader_len(libvalkey_ReaderObject *self) {
    size_t len;
    Py_BEGIN_CRITICAL_SECTION(self);
    len = self->reader->len;
    Py_END_CRITICAL_SECTION();
    return PyLong_FromSize_t(len);
}

static PyObject *Reader_has_data(libvalkey_ReaderObject *self) {
    int has_data;
    Py_BEGIN_CRITICAL_SECTION(self);
    has_data = self->reader->pos < self->reader->len;
    Py_END_CRITICAL_SECTION();
    if (has_data)
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *Reader_set_encoding_impl(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "encoding", "errors", NULL };
    char *encoding = NULL;
    char *errors = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|zz", kwlist, &encoding, &errors))
        return NULL;

    if(_Reader_set_encoding(self, encoding, errors) == -1)
        return NULL;

    Py_RETURN_NONE;

}

static PyObject *Reader_set_encoding(libvalkey_ReaderObject *self, PyObject *args, PyObject *kwds) {
    PyObject *res;
    Py_BEGIN_CRITICAL_SECTION(self);
    res = Reader_set_encoding_impl(self, args, kwds);
    Py_END_CRITICAL_SECTION();
    return res;
}

static PyObject *Reader_convertSetsToLists(PyObject *obj, void *closure) {
    libvalkey_ReaderObject *self = (libvalkey_ReaderObject*)obj;
    int convertSetsToLists;
    Py_BEGIN_CRITICAL_SECTION(self);
    convertSetsToLists = self->convertSetsToLists;
    Py_END_CRITICAL_SECTION();
    /* PyBool_FromLong() returns a new reference already. */
    return PyBool_FromLong(convertSetsToLists);
}
