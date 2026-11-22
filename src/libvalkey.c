#include "libvalkey.h"
#include "reader.h"
#include "pack.h"

static int libvalkey_ModuleTraverse(PyObject *m, visitproc visit, void *arg) {
    Py_VISIT(GET_STATE(m)->VkErr_Base);
    Py_VISIT(GET_STATE(m)->VkErr_ProtocolError);
    Py_VISIT(GET_STATE(m)->VkErr_ReplyError);
    return 0;
}

static int libvalkey_ModuleClear(PyObject *m) {
    Py_CLEAR(GET_STATE(m)->VkErr_Base);
    Py_CLEAR(GET_STATE(m)->VkErr_ProtocolError);
    Py_CLEAR(GET_STATE(m)->VkErr_ReplyError);
    return 0;
}

static PyObject*
py_pack_command(PyObject* self, PyObject* cmd)
{
    return pack_command(cmd);
}

PyDoc_STRVAR(pack_command_doc, "Pack a series of arguments into the Valkey protocol");

PyMethodDef pack_command_method = {
    "pack_command",                 /* The name as a C string. */
    (PyCFunction) py_pack_command,  /* The C function to invoke. */
    METH_O,                         /* Flags telling Python how to invoke */
    pack_command_doc,               /* The docstring as a C string. */
};


PyMethodDef methods[] = {
    {"pack_command", (PyCFunction) py_pack_command, METH_O, pack_command_doc},
    {NULL},
};

static struct PyModuleDef libvalkey_ModuleDef = {
    PyModuleDef_HEAD_INIT,
    MOD_LIBVALKEY,
    NULL,
    sizeof(struct libvalkey_ModuleState), /* m_size */
    methods, /* m_methods */
    NULL, /* m_reload */
    libvalkey_ModuleTraverse, /* m_traverse */
    libvalkey_ModuleClear, /* m_clear */
    NULL /* m_free */
};

/* Keep pointer around for other classes to access the module state. */
PyObject *mod_libvalkey;

/* Adds a new reference to obj to the module, and leaves the caller's own
 * reference alone, which PyModule_AddObject() only does when it succeeds. */
static int add_object(PyObject *mod, const char *name, PyObject *obj) {
    Py_INCREF(obj);
    if (PyModule_AddObject(mod, name, obj) < 0) {
        Py_DECREF(obj);
        return -1;
    }
    return 0;
}

PyMODINIT_FUNC PyInit_libvalkey(void)

{
    PyObject *mod;
    struct libvalkey_ModuleState *state;

    if (PyType_Ready(&libvalkey_ReaderType) < 0) {
        return NULL;
    }

    mod = PyModule_Create(&libvalkey_ModuleDef);
    if (mod == NULL) {
        return NULL;
    }

#ifdef Py_GIL_DISABLED
    /* Everything this extension shares between threads is protected, see
     * "Thread safety" in reader.c. Without this declaration importing it turns
     * the GIL back on. */
    if (PyUnstable_Module_SetGIL(mod, Py_MOD_GIL_NOT_USED) < 0) {
        goto error;
    }
#endif

    /* Setup custom exceptions */
    state = GET_STATE(mod);
    state->VkErr_Base =
        PyErr_NewException(MOD_LIBVALKEY ".LibvalkeyError", PyExc_Exception, NULL);
    if (state->VkErr_Base == NULL) {
        goto error;
    }
    state->VkErr_ProtocolError =
        PyErr_NewException(MOD_LIBVALKEY ".ProtocolError", state->VkErr_Base, NULL);
    if (state->VkErr_ProtocolError == NULL) {
        goto error;
    }
    state->VkErr_ReplyError =
        PyErr_NewException(MOD_LIBVALKEY ".ReplyError", state->VkErr_Base, NULL);
    if (state->VkErr_ReplyError == NULL) {
        goto error;
    }

    if (add_object(mod, "LibvalkeyError", state->VkErr_Base) < 0 ||
        add_object(mod, "ProtocolError", state->VkErr_ProtocolError) < 0 ||
        add_object(mod, "ReplyError", state->VkErr_ReplyError) < 0 ||
        add_object(mod, "Reader", (PyObject *)&libvalkey_ReaderType) < 0) {
        goto error;
    }

    /* Reader() reads the default exceptions from the module state for as long
     * as the Reader type exists, which is as long as this extension is loaded.
     * Hold a reference so that the state cannot go away (for instance when the
     * module is deleted from sys.modules), and publish the pointer only now
     * that the module is complete. */
    Py_INCREF(mod);
    mod_libvalkey = mod;

    return mod;

error:
    Py_DECREF(mod);
    return NULL;
}
