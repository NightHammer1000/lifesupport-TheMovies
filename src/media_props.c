#include "trace.h"
#include "media_props.h"
#include <stdlib.h>
#include <string.h>

static HRESULT STDMETHODCALLTYPE OMP_QI(IWMOutputMediaProps *This, REFIID riid, void **ppv) {
    TRACE_MSG("OMP_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IWMMediaProps) ||
        IsEqualGUID(riid, &IID_IWMOutputMediaProps)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE OMP_AddRef(IWMOutputMediaProps *This) {
    return InterlockedIncrement(&((ProxyOutputMediaProps *)This)->ref_count);
}

static ULONG STDMETHODCALLTYPE OMP_Release(IWMOutputMediaProps *This) {
    TRACE_MSG("OMP_Release");
    ProxyOutputMediaProps *p = (ProxyOutputMediaProps *)This;
    LONG ref = InterlockedDecrement(&p->ref_count);
    if (ref == 0) { free(p->format_buf); free(p); }
    return ref;
}

static HRESULT STDMETHODCALLTYPE OMP_GetType(IWMOutputMediaProps *This, GUID *out) {
    TRACE_MSG("OMP_GetType");
    if (!out) return E_POINTER;
    *out = ((ProxyOutputMediaProps *)This)->media_type.majortype;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OMP_GetMediaType(IWMOutputMediaProps *This, WM_MEDIA_TYPE *pType, DWORD *pcb) {
    TRACE_MSG("OMP_GetMediaType");
    ProxyOutputMediaProps *p = (ProxyOutputMediaProps *)This;
    if (!pcb) return E_POINTER;
    DWORD needed = sizeof(WM_MEDIA_TYPE) + p->media_type.cbFormat;
    if (!pType) { *pcb = needed; return S_OK; }
    if (*pcb < needed) { *pcb = needed; return ASF_E_BUFFERTOOSMALL; }

    *pType = p->media_type;
    if (p->media_type.cbFormat > 0 && p->format_buf) {
        BYTE *dest = ((BYTE *)pType) + sizeof(WM_MEDIA_TYPE);
        memcpy(dest, p->format_buf, p->media_type.cbFormat);
        pType->pbFormat = dest;
    } else {
        pType->pbFormat = NULL;
    }
    pType->pUnk = NULL;
    *pcb = needed;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OMP_SetMediaType(IWMOutputMediaProps *This, WM_MEDIA_TYPE *pType) {
    TRACE_MSG("OMP_SetMediaType");
    ProxyOutputMediaProps *p = (ProxyOutputMediaProps *)This;
    if (!pType) return E_POINTER;
    free(p->format_buf);
    p->format_buf = NULL;
    p->media_type = *pType;
    if (pType->cbFormat > 0 && pType->pbFormat) {
        p->format_buf = malloc(pType->cbFormat);
        if (!p->format_buf) return E_OUTOFMEMORY;
        memcpy(p->format_buf, pType->pbFormat, pType->cbFormat);
    }
    p->media_type.pbFormat = p->format_buf;
    p->media_type.pUnk = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OMP_GetStreamGroupName(IWMOutputMediaProps *This, WCHAR *name, WORD *len) {
    TRACE_MSG("OMP_GetStreamGroupName");
    if (!len) return E_POINTER;
    if (!name) { *len = 1; return S_OK; }
    name[0] = L'\0'; *len = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OMP_GetConnectionName(IWMOutputMediaProps *This, WCHAR *name, WORD *len) {
    TRACE_MSG("OMP_GetConnectionName");
    if (!len) return E_POINTER;
    if (!name) { *len = 1; return S_OK; }
    name[0] = L'\0'; *len = 1;
    return S_OK;
}

static IWMOutputMediaPropsVtbl g_OMPVtbl = {
    OMP_QI, OMP_AddRef, OMP_Release,
    OMP_GetType, OMP_GetMediaType, OMP_SetMediaType,
    OMP_GetStreamGroupName, OMP_GetConnectionName
};

ProxyOutputMediaProps *output_media_props_create(const WM_MEDIA_TYPE *type) {
    ProxyOutputMediaProps *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->lpVtbl = &g_OMPVtbl;
    p->ref_count = 1;
    if (type) {
        p->media_type = *type;
        if (type->cbFormat > 0 && type->pbFormat) {
            p->format_buf = malloc(type->cbFormat);
            if (!p->format_buf) { free(p); return NULL; }
            memcpy(p->format_buf, type->pbFormat, type->cbFormat);
            p->media_type.pbFormat = p->format_buf;
        }
        p->media_type.pUnk = NULL;
    }
    return p;
}
