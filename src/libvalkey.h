#ifndef __LIBVALKEY_PY_H
#define __LIBVALKEY_PY_H

#include <Python.h>
#include <valkey/read.h>

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif

/* Per-object critical sections. CPython 3.13+ provides them, and they only do
 * something in free-threaded builds. Anywhere else (older CPython, PyPy) the GIL
 * already serializes access and they are no-ops. Never leave the block between
 * them with return or goto, or the lock is not released. */
#ifndef Py_BEGIN_CRITICAL_SECTION
#define Py_BEGIN_CRITICAL_SECTION(op) {
#define Py_END_CRITICAL_SECTION() }
#endif

#ifndef MOD_LIBVALKEY
#define MOD_LIBVALKEY "libvalkey"
#endif

struct libvalkey_ModuleState {
    PyObject *VkErr_Base;
    PyObject *VkErr_ProtocolError;
    PyObject *VkErr_ReplyError;
};

#define GET_STATE(__s) ((struct libvalkey_ModuleState*)PyModule_GetState(__s))

/* Keep pointer around for other classes to access the module state. */
extern PyObject *mod_libvalkey;
#define LIBVALKEY_STATE (GET_STATE(mod_libvalkey))

PyMODINIT_FUNC PyInit_libvalkey(void);

#endif
