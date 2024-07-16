#ifndef __READER_H
#define __READER_H

#include "valkey/valkey.h"
#include <Python.h>

typedef struct {
    PyObject_HEAD
    valkeyReader *reader;
    char *encoding;
    char *errors;
    int shouldDecode;
    PyObject *protocolErrorClass;
    PyObject *replyErrorClass;
    PyObject *notEnoughDataObject;
    int listOnly;

    /* Stores error object in between incomplete calls to #gets, in order to
     * only set the error once a full reply has been read. Otherwise, the
     * reader could get in an inconsistent state. */
    struct {
        PyObject *ptype;
        PyObject *pvalue;
        PyObject *ptraceback;
    } error;
} libvalkey_ReaderObject;

extern PyTypeObject libvalkey_ReaderType;
extern valkeyReplyObjectFunctions libvalkey_ObjectFunctions;

#endif
