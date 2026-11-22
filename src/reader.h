#ifndef __READER_H
#define __READER_H

#include "valkey/valkey.h"
#include <Python.h>

typedef struct {
    PyObject_HEAD
    valkeyReader *reader;
    /* The codec and error handler names, as bytes objects that are replaced and
     * never modified. NULL means no decoding, and "strict" respectively. */
    PyObject *encoding;
    PyObject *errors;
    int shouldDecode;
    /* Set while gets() runs, see "Thread safety" in reader.c. */
    int busy;
    PyObject *protocolErrorClass;
    PyObject *replyErrorClass;
    PyObject *notEnoughDataObject;
    int convertSetsToLists;

    PyObject *pendingObject;

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
