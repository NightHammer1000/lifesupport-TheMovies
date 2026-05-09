#include "ds_output_pin.h"
#include "ds_filter.h"
#include "log.h"
#include "trace.h"
#include <stdlib.h>
#include <string.h>

/* ========== IEnumMediaTypes ========== */

typedef struct PinEnumMT {
    IEnumMediaTypesVtbl_DS *lpVtbl;
    LONG       ref_count;
    DSOutputPin *pin;
    int         index;
} PinEnumMT;

static HRESULT STDMETHODCALLTYPE EMT_QI(IEnumMediaTypes_DS *This, REFIID riid, void **ppv) {
    TRACE_MSG("EMT_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumMediaTypes)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EMT_AddRef(IEnumMediaTypes_DS *This) {
    return InterlockedIncrement(&((PinEnumMT*)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE EMT_Release(IEnumMediaTypes_DS *This) {
    TRACE_MSG("EMT_Release");
    PinEnumMT *e = (PinEnumMT*)This;
    LONG ref = InterlockedDecrement(&e->ref_count);
    if (ref == 0) free(e);
    return ref;
}
static HRESULT STDMETHODCALLTYPE EMT_Next(IEnumMediaTypes_DS *This, ULONG count, AM_MEDIA_TYPE **out, ULONG *fetched) {
    TRACE_MSG("EMT_Next");
    PinEnumMT *e = (PinEnumMT*)This;
    ULONG got = 0;
    while (got < count && e->index < 1) {
        AM_MEDIA_TYPE mt;
        ds_output_pin_get_media_type(e->pin, &mt);
        out[got] = media_type_clone(&mt);
        if (mt.pbFormat) free(mt.pbFormat);
        e->index++;
        got++;
    }
    if (fetched) *fetched = got;
    return (got == count) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EMT_Skip(IEnumMediaTypes_DS *This, ULONG count) {
    TRACE_MSG("EMT_Skip");
    ((PinEnumMT*)This)->index += count; return S_OK;
}
static HRESULT STDMETHODCALLTYPE EMT_Reset(IEnumMediaTypes_DS *This) {
    TRACE_MSG("EMT_Reset");
    ((PinEnumMT*)This)->index = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE EMT_Clone(IEnumMediaTypes_DS *This, IEnumMediaTypes_DS **ppEnum) {
    return E_NOTIMPL;
}

static IEnumMediaTypesVtbl_DS g_EMTVtbl = {
    EMT_QI, EMT_AddRef, EMT_Release,
    EMT_Next, EMT_Skip, EMT_Reset, EMT_Clone
};

static PinEnumMT *enum_mt_create(DSOutputPin *pin) {
    PinEnumMT *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->lpVtbl = &g_EMTVtbl;
    e->ref_count = 1;
    e->pin = pin;
    return e;
}

/* ========== Media type helpers ========== */

void ds_output_pin_get_media_type(DSOutputPin *pin, AM_MEDIA_TYPE *mt) {
    memset(mt, 0, sizeof(*mt));
    VIDEOINFOHEADER *vih = calloc(1, sizeof(VIDEOINFOHEADER));
    mt->majortype = WMMEDIATYPE_Video;
    mt->subtype = WMMEDIASUBTYPE_RGB24;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = pin->width * pin->height * 3;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE*)vih;
    vih->AvgTimePerFrame = 333333; /* ~30 fps placeholder; real pacing is libmpv's */
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = pin->width;
    vih->bmiHeader.biHeight = pin->height;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = 24;
    vih->bmiHeader.biCompression = BI_RGB;
    vih->bmiHeader.biSizeImage = mt->lSampleSize;
}

/* ========== IPin implementation ========== */

/* Standalone IMediaSeeking wrapper — allocated on heap so it looks like
   a normal COM object to quartz.dll (not embedded in another struct) */
typedef struct PinMediaSeeking {
    IMediaSeekingVtbl_DS *lpVtbl;
    LONG ref_count;
    DSOutputPin *pin;
} PinMediaSeeking;

static HRESULT STDMETHODCALLTYPE PMS_QI(IMediaSeeking_DS *This, REFIID riid, void **ppv) {
    TRACE_MSG("PMS_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IMediaSeeking)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE PMS_AddRef(IMediaSeeking_DS *This) {
    return InterlockedIncrement(&((PinMediaSeeking*)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE PMS_Release(IMediaSeeking_DS *This) {
    TRACE_MSG("PMS_Release");
    PinMediaSeeking *s = (PinMediaSeeking*)This;
    LONG ref = InterlockedDecrement(&s->ref_count);
    if (ref == 0) free(s);
    return ref;
}

/* Forward to filter's data for all seeking methods */
#define PMS_FILTER(This) (((PinMediaSeeking*)(This))->pin->filter)
#define PMS_CAPS (AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanSeekForwards | \
                  AM_SEEKING_CanSeekBackwards | AM_SEEKING_CanGetCurrentPos | \
                  AM_SEEKING_CanGetDuration | AM_SEEKING_CanGetStopPos)

static HRESULT STDMETHODCALLTYPE PMS_GetCapabilities(IMediaSeeking_DS *t, DWORD *p) { if(p)*p=PMS_CAPS; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_CheckCapabilities(IMediaSeeking_DS *t, DWORD *p) {
    TRACE_MSG("PMS_CheckCapabilities");
    if(!p)return E_POINTER; DWORD a=*p; *p=a&PMS_CAPS; return(*p==a)?S_OK:(*p?S_FALSE:E_FAIL);
}
static HRESULT STDMETHODCALLTYPE PMS_IsFormatSupported(IMediaSeeking_DS *t, const GUID *f) {
    return IsEqualGUID(f,&TIME_FORMAT_MEDIA_TIME)?S_OK:S_FALSE;
}
static HRESULT STDMETHODCALLTYPE PMS_QueryPreferredFormat(IMediaSeeking_DS *t, GUID *p) { if(p)*p=TIME_FORMAT_MEDIA_TIME; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_GetTimeFormat(IMediaSeeking_DS *t, GUID *p) { if(p)*p=TIME_FORMAT_MEDIA_TIME; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_IsUsingTimeFormat(IMediaSeeking_DS *t, const GUID *p) {
    return IsEqualGUID(p,&TIME_FORMAT_MEDIA_TIME)?S_OK:S_FALSE;
}
static HRESULT STDMETHODCALLTYPE PMS_SetTimeFormat(IMediaSeeking_DS *t, const GUID *p) {
    return IsEqualGUID(p,&TIME_FORMAT_MEDIA_TIME)?S_OK:E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE PMS_GetDuration(IMediaSeeking_DS *t, LONGLONG *p) {
    /* Access filter duration through the pin's back-pointer */
    if(!p) return E_POINTER;
    /* We can't directly access DSSourceFilter fields from here without the header,
       but the pin stores the filter pointer. For now return 0 — will be connected later */
    *p = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE PMS_GetStopPosition(IMediaSeeking_DS *t, LONGLONG *p) { return PMS_GetDuration(t,p); }
static HRESULT STDMETHODCALLTYPE PMS_GetCurrentPosition(IMediaSeeking_DS *t, LONGLONG *p) { if(p)*p=0; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_ConvertTimeFormat(IMediaSeeking_DS *t, LONGLONG *a,const GUID *b,LONGLONG c,const GUID *d) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE PMS_SetPositions(IMediaSeeking_DS *t, LONGLONG *a,DWORD b,LONGLONG *c,DWORD d) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_GetPositions(IMediaSeeking_DS *t, LONGLONG *a,LONGLONG *b) { if(a)*a=0;if(b)*b=0; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_GetAvailable(IMediaSeeking_DS *t, LONGLONG *a,LONGLONG *b) { if(a)*a=0;if(b)*b=0; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_SetRate(IMediaSeeking_DS *t, double r) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_GetRate(IMediaSeeking_DS *t, double *p) { if(p)*p=1.0; return S_OK; }
static HRESULT STDMETHODCALLTYPE PMS_GetPreroll(IMediaSeeking_DS *t, LONGLONG *p) { if(p)*p=0; return S_OK; }

static IMediaSeekingVtbl_DS g_PinMSVtbl = {
    PMS_QI, PMS_AddRef, PMS_Release,
    PMS_GetCapabilities, PMS_CheckCapabilities,
    PMS_IsFormatSupported, PMS_QueryPreferredFormat,
    PMS_GetTimeFormat, PMS_IsUsingTimeFormat, PMS_SetTimeFormat,
    PMS_GetDuration, PMS_GetStopPosition, PMS_GetCurrentPosition,
    PMS_ConvertTimeFormat, PMS_SetPositions, PMS_GetPositions,
    PMS_GetAvailable, PMS_SetRate, PMS_GetRate, PMS_GetPreroll
};

static HRESULT STDMETHODCALLTYPE Pin_QI(IPin_DS *This, REFIID riid, void **ppv) {
    TRACE_MSG("Pin_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPin)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IMediaSeeking)) {
        PinMediaSeeking *s = calloc(1, sizeof(*s));
        if (!s) return E_OUTOFMEMORY;
        s->lpVtbl = &g_PinMSVtbl;
        s->ref_count = 1;
        s->pin = (DSOutputPin*)This;
        *ppv = s;
        proxy_log("Pin[%ls]::QI -> standalone IMediaSeeking %p", ((DSOutputPin*)This)->pin_id, s);
        return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Pin_AddRef(IPin_DS *This) {
    return InterlockedIncrement(&((DSOutputPin*)This)->ref_count);
}

static ULONG STDMETHODCALLTYPE Pin_Release(IPin_DS *This) {
    TRACE_MSG("Pin_Release");
    DSOutputPin *pin = (DSOutputPin*)This;
    LONG ref = InterlockedDecrement(&pin->ref_count);
    if (ref == 0) {
        if (pin->peer) pin->peer->lpVtbl->Release(pin->peer);
        if (pin->peer_mem) pin->peer_mem->lpVtbl->Release(pin->peer_mem);
        if (pin->allocator) pin->allocator->lpVtbl->Release(pin->allocator);
        free(pin->mt_format_buf);
        free(pin);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE Pin_Connect(IPin_DS *This, IPin_DS *pReceivePin, const AM_MEDIA_TYPE *pmt) {
    TRACE_MSG("Pin_Connect");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (pin->connected) return E_FAIL;
    proxy_log("Pin[%ls]::Connect()", pin->pin_id);

    AM_MEDIA_TYPE mt;
    ds_output_pin_get_media_type(pin, &mt);

    HRESULT hr = pReceivePin->lpVtbl->ReceiveConnection(pReceivePin, This, &mt);
    if (hr < 0) {
        proxy_log("  ReceiveConnection failed: 0x%08lX", hr);
        free(mt.pbFormat);
        return hr;
    }

    /* Store negotiated type */
    pin->mt = mt;
    pin->mt.pbFormat = NULL;
    free(pin->mt_format_buf);
    pin->mt_format_buf = mt.pbFormat;
    pin->mt.pbFormat = pin->mt_format_buf;

    /* Get IMemInputPin from downstream */
    hr = pReceivePin->lpVtbl->QueryInterface(pReceivePin, &IID_IMemInputPin, (void**)&pin->peer_mem);
    if (hr < 0) {
        proxy_log("  QI(IMemInputPin) failed");
        pReceivePin->lpVtbl->Disconnect(pReceivePin);
        return hr;
    }

    /* Get allocator from downstream */
    hr = pin->peer_mem->lpVtbl->GetAllocator(pin->peer_mem, &pin->allocator);
    if (hr < 0) {
        proxy_log("  GetAllocator failed — no fallback yet");
        pin->peer_mem->lpVtbl->Release(pin->peer_mem);
        pin->peer_mem = NULL;
        pReceivePin->lpVtbl->Disconnect(pReceivePin);
        return hr;
    }

    /* Configure allocator */
    ALLOCATOR_PROPERTIES req = {0}, actual = {0};
    req.cBuffers = 4;
    req.cbBuffer = mt.lSampleSize;
    req.cbAlign = 1;
    req.cbPrefix = 0;
    pin->allocator->lpVtbl->SetProperties(pin->allocator, &req, &actual);
    pin->peer_mem->lpVtbl->NotifyAllocator(pin->peer_mem, pin->allocator, FALSE);

    pReceivePin->lpVtbl->AddRef(pReceivePin);
    pin->peer = pReceivePin;
    pin->connected = TRUE;

    proxy_log("  Pin[%ls] connected: %ldx%ld buffers (peer=%p peer_mem=%p alloc=%p)",
              pin->pin_id, actual.cBuffers, actual.cbBuffer,
              pin->peer, pin->peer_mem, pin->allocator);

    /* Log what the downstream peer is */
    PIN_INFO peer_info = {0};
    pReceivePin->lpVtbl->QueryPinInfo(pReceivePin, &peer_info);
    proxy_log("  Downstream: filter=%p pin_name=%ls", peer_info.pFilter, peer_info.achName);
    if (peer_info.pFilter)
        ((IBaseFilter_DS*)peer_info.pFilter)->lpVtbl->Release((IBaseFilter_DS*)peer_info.pFilter);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ReceiveConnection(IPin_DS *This, IPin_DS *p, const AM_MEDIA_TYPE *mt) {
    return E_UNEXPECTED;
}

static HRESULT STDMETHODCALLTYPE Pin_Disconnect(IPin_DS *This) {
    TRACE_MSG("Pin_Disconnect");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (!pin->connected) return S_FALSE;
    if (pin->allocator) { pin->allocator->lpVtbl->Release(pin->allocator); pin->allocator = NULL; }
    if (pin->peer_mem) { pin->peer_mem->lpVtbl->Release(pin->peer_mem); pin->peer_mem = NULL; }
    if (pin->peer) { pin->peer->lpVtbl->Release(pin->peer); pin->peer = NULL; }
    pin->connected = FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ConnectedTo(IPin_DS *This, IPin_DS **ppPin) {
    TRACE_MSG("Pin_ConnectedTo");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (!ppPin) return E_POINTER;
    if (!pin->connected) { *ppPin = NULL; return VFW_E_NOT_CONNECTED; }
    *ppPin = pin->peer;
    pin->peer->lpVtbl->AddRef(pin->peer);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ConnectionMediaType(IPin_DS *This, AM_MEDIA_TYPE *pmt) {
    TRACE_MSG("Pin_ConnectionMediaType");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (!pmt) return E_POINTER;
    if (!pin->connected) return VFW_E_NOT_CONNECTED;
    *pmt = pin->mt;
    pmt->pUnk = NULL;
    if (pin->mt.cbFormat > 0 && pin->mt_format_buf) {
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(pin->mt.cbFormat);
        if (pmt->pbFormat) memcpy(pmt->pbFormat, pin->mt_format_buf, pin->mt.cbFormat);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryPinInfo(IPin_DS *This, PIN_INFO *pInfo) {
    TRACE_MSG("Pin_QueryPinInfo");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = (IBaseFilter_DS*)pin->filter;
    ((IBaseFilter_DS*)pin->filter)->lpVtbl->AddRef((IBaseFilter_DS*)pin->filter);
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy(pInfo->achName, pin->pin_id);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryDirection(IPin_DS *This, PIN_DIRECTION *pDir) {
    TRACE_MSG("Pin_QueryDirection");
    if (!pDir) return E_POINTER;
    *pDir = PINDIR_OUTPUT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryId(IPin_DS *This, LPWSTR *pId) {
    TRACE_MSG("Pin_QueryId");
    DSOutputPin *pin = (DSOutputPin*)This;
    if (!pId) return E_POINTER;
    size_t len = (wcslen(pin->pin_id) + 1) * sizeof(WCHAR);
    *pId = (LPWSTR)CoTaskMemAlloc(len);
    if (!*pId) return E_OUTOFMEMORY;
    memcpy(*pId, pin->pin_id, len);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryAccept(IPin_DS *This, const AM_MEDIA_TYPE *pmt) {
    TRACE_MSG("Pin_QueryAccept");
    return IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Video) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE Pin_EnumMediaTypes(IPin_DS *This, IEnumMediaTypes_DS **ppEnum) {
    TRACE_MSG("Pin_EnumMediaTypes");
    if (!ppEnum) return E_POINTER;
    PinEnumMT *e = enum_mt_create((DSOutputPin*)This);
    if (!e) return E_OUTOFMEMORY;
    *ppEnum = (IEnumMediaTypes_DS*)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryInternalConnections(IPin_DS *This, IPin_DS **p, ULONG *n) {
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Pin_EndOfStream(IPin_DS *This) { return E_UNEXPECTED; }
static HRESULT STDMETHODCALLTYPE Pin_BeginFlush(IPin_DS *This) { return E_UNEXPECTED; }
static HRESULT STDMETHODCALLTYPE Pin_EndFlush(IPin_DS *This) { return E_UNEXPECTED; }
static HRESULT STDMETHODCALLTYPE Pin_NewSegment(IPin_DS *This, REFERENCE_TIME s, REFERENCE_TIME e, double r) {
    return S_OK;
}

static IPinVtbl_DS g_PinVtbl = {
    Pin_QI, Pin_AddRef, Pin_Release,
    Pin_Connect, Pin_ReceiveConnection, Pin_Disconnect,
    Pin_ConnectedTo, Pin_ConnectionMediaType,
    Pin_QueryPinInfo, Pin_QueryDirection, Pin_QueryId,
    Pin_QueryAccept, Pin_EnumMediaTypes,
    Pin_QueryInternalConnections,
    Pin_EndOfStream, Pin_BeginFlush, Pin_EndFlush, Pin_NewSegment
};

/* ========== Delivery helpers (called from filter's worker thread) ========== */

HRESULT ds_output_pin_deliver(DSOutputPin *pin, BYTE *data, long size,
                              REFERENCE_TIME start, REFERENCE_TIME end) {
    if (!pin->connected || !pin->peer_mem || !pin->allocator) return S_FALSE;
    if (pin->flushing) return S_FALSE;

    IMediaSample_DS *sample = NULL;
    HRESULT hr = pin->allocator->lpVtbl->GetBuffer(pin->allocator, &sample, NULL, NULL, 0);
    if (hr < 0 || !sample) return hr;

    BYTE *buf = NULL;
    sample->lpVtbl->GetPointer(sample, &buf);
    long max_size = sample->lpVtbl->GetSize(sample);
    if (size > max_size) size = max_size;
    memcpy(buf, data, size);

    sample->lpVtbl->SetActualDataLength(sample, size);
    sample->lpVtbl->SetTime(sample, &start, &end);
    sample->lpVtbl->SetSyncPoint(sample, TRUE);
    sample->lpVtbl->SetPreroll(sample, FALSE);
    sample->lpVtbl->SetDiscontinuity(sample, FALSE);

    hr = pin->peer_mem->lpVtbl->Receive(pin->peer_mem, sample);
    sample->lpVtbl->Release(sample);
    pin->current_time = end;
    return hr;
}

/* ========== Factory ========== */

DSOutputPin *ds_output_pin_create(DSSourceFilter *filter, int width, int height) {
    DSOutputPin *pin = calloc(1, sizeof(*pin));
    if (!pin) return NULL;
    pin->lpVtbl = &g_PinVtbl;
    pin->ref_count = 1;
    pin->filter = filter;
    pin->width  = width;
    pin->height = height;
    wcscpy(pin->pin_id, L"Video");
    proxy_log("Created DSOutputPin[Video] %dx%d", width, height);
    return pin;
}
