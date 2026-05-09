#include "trace.h"
#include "nss_buffer.h"
#include <stdlib.h>

static HRESULT STDMETHODCALLTYPE NB_QueryInterface(INSSBuffer *This, REFIID riid, void **ppv) {
    TRACE_MSG("NB_QueryInterface");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_INSSBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE NB_AddRef(INSSBuffer *This) {
    return InterlockedIncrement(&((ProxyNSSBuffer *)This)->ref_count);
}

static ULONG STDMETHODCALLTYPE NB_Release(INSSBuffer *This) {
    TRACE_MSG("NB_Release");
    ProxyNSSBuffer *b = (ProxyNSSBuffer *)This;
    LONG ref = InterlockedDecrement(&b->ref_count);
    if (ref == 0) { free(b->data); free(b); }
    return ref;
}

static HRESULT STDMETHODCALLTYPE NB_GetLength(INSSBuffer *This, DWORD *out) {
    TRACE_MSG("NB_GetLength");
    if (!out) return E_POINTER;
    *out = ((ProxyNSSBuffer *)This)->length;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE NB_SetLength(INSSBuffer *This, DWORD len) {
    TRACE_MSG("NB_SetLength");
    ProxyNSSBuffer *b = (ProxyNSSBuffer *)This;
    if (len > b->max_length) return E_INVALIDARG;
    b->length = len;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE NB_GetMaxLength(INSSBuffer *This, DWORD *out) {
    TRACE_MSG("NB_GetMaxLength");
    if (!out) return E_POINTER;
    *out = ((ProxyNSSBuffer *)This)->max_length;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE NB_GetBuffer(INSSBuffer *This, BYTE **out) {
    TRACE_MSG("NB_GetBuffer");
    if (!out) return E_POINTER;
    *out = ((ProxyNSSBuffer *)This)->data;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE NB_GetBufferAndLength(INSSBuffer *This, BYTE **buf, DWORD *len) {
    TRACE_MSG("NB_GetBufferAndLength");
    ProxyNSSBuffer *b = (ProxyNSSBuffer *)This;
    if (buf) *buf = b->data;
    if (len) *len = b->length;
    return S_OK;
}

static INSSBufferVtbl g_NSSBufferVtbl = {
    NB_QueryInterface, NB_AddRef, NB_Release,
    NB_GetLength, NB_SetLength, NB_GetMaxLength,
    NB_GetBuffer, NB_GetBufferAndLength
};

ProxyNSSBuffer *nss_buffer_create(DWORD max_size) {
    ProxyNSSBuffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->lpVtbl = &g_NSSBufferVtbl;
    b->ref_count = 1;
    b->data = calloc(1, max_size);
    if (!b->data) { free(b); return NULL; }
    b->max_length = max_size;
    return b;
}
