#include "pack.h"
#include "sds.h"

#include <limits.h>

#ifndef _MSC_VER
#include <valkey/valkey.h>
#else
/* Workaround for https://bugs.python.org/issue11717.
 * <valkey/valkey.h> defines ssize_t which can conflict
 * with Python's definition.
 */
// TODO: remove this workaround when Python 3.10 is required
extern long long valkeyFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen);
typedef char *sds;
extern void sds_free(void *ptr);
extern sds sdsempty(void);
extern void sdsfreesplitres(sds *tokens, int count);
extern sds sdscpylen(sds s, const char *t, size_t len);
extern sds sdsnewlen(const void *init, size_t initlen);
#endif

#include <valkey/alloc.h>

/* Safe to call from several threads at once: it only reads its argument, which
 * the caller keeps alive, and the items of a tuple cannot change. Items that
 * can change, a memoryview, are copied while an export holds their buffer. */
PyObject *
pack_command(PyObject *cmd)
{
    PyObject *result = NULL;

    if (cmd == NULL || !PyTuple_Check(cmd))
    {
        PyErr_SetString(PyExc_TypeError,
                        "The argument must be a tuple of str, int, float or bytes.");
        return NULL;
    }

    Py_ssize_t tokens_number = PyTuple_GET_SIZE(cmd);
    if (tokens_number > INT_MAX)
    {
        PyErr_SetString(PyExc_OverflowError, "Too many arguments in the command.");
        return NULL;
    }

    /* One more than needed, so that an empty command does not ask for 0 bytes. */
    sds *tokens = vk_calloc(tokens_number + 1, sizeof(sds));
    size_t *lengths = vk_malloc(sizeof(size_t) * (tokens_number + 1));
    char *resp_bytes = NULL;
    if (tokens == NULL || lengths == NULL)
    {
        PyErr_NoMemory();
        goto cleanup;
    }

    Py_ssize_t len = 0;
    for (Py_ssize_t i = 0; i < tokens_number; i++)
    {
        PyObject *item = PyTuple_GET_ITEM(cmd, i);

        if (PyBytes_Check(item))
        {
            len = PyBytes_GET_SIZE(item);
            tokens[i] = sdsnewlen(PyBytes_AS_STRING(item), len);
            lengths[i] = len;
        }
        else if (PyUnicode_Check(item))
        {
            const char *bytes = PyUnicode_AsUTF8AndSize(item, &len);
            if (bytes == NULL)
            {
                // PyUnicode_AsUTF8AndSize sets an exception.
                goto cleanup;
            }

            tokens[i] = sdsnewlen(bytes, len);
            lengths[i] = len;
        }
        else if (PyMemoryView_Check(item))
        {
            /* Ask for the buffer rather than reading the memoryview's own copy
             * of it: that is how a released memoryview raises ValueError and a
             * non-contiguous one BufferError, where the copy would be read
             * from memory that is gone or is not laid out as one block. The
             * export also keeps another thread from releasing the memoryview
             * or resizing what it views while it is copied. */
            Py_buffer view;
            if (PyObject_GetBuffer(item, &view, PyBUF_SIMPLE) < 0)
            {
                goto cleanup;
            }

            tokens[i] = sdsnewlen(view.buf, view.len);
            lengths[i] = view.len;
            PyBuffer_Release(&view);
        }
        else
        {
            if (PyLong_CheckExact(item) || PyFloat_Check(item))
            {
                /* Fails for an int with more digits than
                 * sys.get_int_max_str_digits() allows, and for a float
                 * subclass with a __repr__ that raises. */
                PyObject *repr = PyObject_Repr(item);
                if (repr == NULL)
                {
                    goto cleanup;
                }

                const char *bytes = PyUnicode_AsUTF8AndSize(repr, &len);
                if (bytes == NULL)
                {
                    Py_DECREF(repr);
                    goto cleanup;
                }

                tokens[i] = sdsnewlen(bytes, len);
                lengths[i] = len;
                Py_DECREF(repr);
            }
            else
            {
                PyErr_SetString(PyExc_TypeError,
                                "A tuple item must be str, int, float or bytes.");
                goto cleanup;
            }
        }

        if (tokens[i] == NULL)
        {
            PyErr_NoMemory();
            goto cleanup;
        }
    }

    len = valkeyFormatCommandArgv(&resp_bytes, (int)tokens_number, (const char **)tokens, lengths);

    if (len == -1)
    {
        PyErr_SetString(PyExc_RuntimeError,
                        "Failed to serialize the command.");
        goto cleanup;
    }

    result = PyBytes_FromStringAndSize(resp_bytes, len);
    vk_free(resp_bytes);
cleanup:
    /* The entries that were not reached are NULL, which sdsfree() accepts. */
    sdsfreesplitres(tokens, (int)tokens_number);
    vk_free(lengths);
    return result;
}
