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

PyMODINIT_FUNC PyInit_libvalkey(void)

{
    if (PyType_Ready(&libvalkey_ReaderType) < 0) {
        return NULL;
    }

    mod_libvalkey= PyModule_Create(&libvalkey_ModuleDef);

    /* Setup custom exceptions */
    LIBVALKEY_STATE->VkErr_Base =
        PyErr_NewException(MOD_LIBVALKEY ".LibvalkeyError", PyExc_Exception, NULL);
    LIBVALKEY_STATE->VkErr_ProtocolError =
        PyErr_NewException(MOD_LIBVALKEY ".ProtocolError", LIBVALKEY_STATE->VkErr_Base, NULL);
    LIBVALKEY_STATE->VkErr_ReplyError =
        PyErr_NewException(MOD_LIBVALKEY ".ReplyError", LIBVALKEY_STATE->VkErr_Base, NULL);

    Py_INCREF(LIBVALKEY_STATE->VkErr_Base);
    PyModule_AddObject(mod_libvalkey, "LibvalkeyError", LIBVALKEY_STATE->VkErr_Base);
    Py_INCREF(LIBVALKEY_STATE->VkErr_ProtocolError);
    PyModule_AddObject(mod_libvalkey, "ProtocolError", LIBVALKEY_STATE->VkErr_ProtocolError);
    Py_INCREF(LIBVALKEY_STATE->VkErr_ReplyError);
    PyModule_AddObject(mod_libvalkey, "ReplyError", LIBVALKEY_STATE->VkErr_ReplyError);

    Py_INCREF(&libvalkey_ReaderType);
    PyModule_AddObject(mod_libvalkey, "Reader", (PyObject *)&libvalkey_ReaderType);

    return mod_libvalkey;
}
