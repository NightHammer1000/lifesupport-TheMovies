#include "ds_filter.h"
#include "ds_output_pin.h"
#include "mpv_player.h"
#include "log.h"
#include "trace.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define MAX_DS_PINS 4

/* ========== DSSourceFilter struct ========== */

/* IMediaPosition vtable (IDispatch + 11 methods) */
typedef struct IMediaPositionVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(void*, UINT*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(void*, UINT, LCID, void**);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(void*, REFIID, LPOLESTR*, UINT, LCID, DISPID*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(void*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*);
    HRESULT (STDMETHODCALLTYPE *get_Duration)(void*, double*);
    HRESULT (STDMETHODCALLTYPE *put_CurrentPosition)(void*, double);
    HRESULT (STDMETHODCALLTYPE *get_CurrentPosition)(void*, double*);
    HRESULT (STDMETHODCALLTYPE *get_StopTime)(void*, double*);
    HRESULT (STDMETHODCALLTYPE *put_StopTime)(void*, double);
    HRESULT (STDMETHODCALLTYPE *get_PrerollTime)(void*, double*);
    HRESULT (STDMETHODCALLTYPE *put_PrerollTime)(void*, double);
    HRESULT (STDMETHODCALLTYPE *put_Rate)(void*, double);
    HRESULT (STDMETHODCALLTYPE *get_Rate)(void*, double*);
    HRESULT (STDMETHODCALLTYPE *CanSeekForward)(void*, long*);
    HRESULT (STDMETHODCALLTYPE *CanSeekBackward)(void*, long*);
} IMediaPositionVtbl_DS;

struct DSSourceFilter {
    /* COM identity */
    IBaseFilterVtbl_DS       *lpVtbl;
    IFileSourceFilterVtbl_DS *lpFileSourceVtbl;
    IMediaSeekingVtbl_DS     *lpMediaSeekingVtbl;
    IBasicAudioVtbl_DS       *lpBasicAudioVtbl;
    IMediaPositionVtbl_DS    *lpMediaPositionVtbl;
    LONG                      ref_count;

    /* Filter graph context */
    IFilterGraph_DS          *graph;
    IReferenceClock_DS       *clock;
    WCHAR                    *filename;
    WCHAR                     filter_name[128];
    FILTER_STATE              state;
    REFERENCE_TIME            start_time;

    /* Pins (one video pin) */
    DSOutputPin              *pins[MAX_DS_PINS];
    int                       pin_count;

    /* libmpv backend */
    mpv_player_t             *player;
    BYTE                     *bgr24_buf;
    size_t                    bgr24_size;
    LONGLONG                  duration_100ns;
    REFERENCE_TIME            frame_dur_100ns;
    long                      audio_volume; /* IBasicAudio scale */

    /* First-frame signalling (for FakeGraph synchronisation) */
    HANDLE                    first_frame_event;
    BOOL                      first_frame_delivered;
    volatile LONGLONG         current_pos_100ns;

    BOOL                      running;
    CRITICAL_SECTION          cs;
};

/* Offset helpers for multi-interface QI */
#define FILTER_FROM_FILESOURCE(p) \
    ((DSSourceFilter*)((BYTE*)(p) - offsetof(DSSourceFilter, lpFileSourceVtbl)))
#define FILTER_FROM_SEEKING(p) \
    ((DSSourceFilter*)((BYTE*)(p) - offsetof(DSSourceFilter, lpMediaSeekingVtbl)))
#define FILTER_FROM_BASICAUDIO(p) \
    ((DSSourceFilter*)((BYTE*)(p) - offsetof(DSSourceFilter, lpBasicAudioVtbl)))
#define FILTER_FROM_MEDIAPOS(p) \
    ((DSSourceFilter*)((BYTE*)(p) - offsetof(DSSourceFilter, lpMediaPositionVtbl)))

/* ========== libmpv frame callback ==========
   Convert BGR0 (top-down, stride=stride_bytes) into BGR24 (bottom-up,
   stride=w*3) and hand it to the output pin. */
static HRESULT on_mpv_frame(void *user, const BYTE *bgr0, int w, int h,
                            int stride_bytes, REFERENCE_TIME pts)
{
    DSSourceFilter *f = (DSSourceFilter*)user;
    if (f->pin_count == 0 || !f->pins[0]) return S_OK;

    size_t need = (size_t)w * (size_t)h * 3u;
    if (need > f->bgr24_size) {
        BYTE *nb = realloc(f->bgr24_buf, need);
        if (!nb) return E_OUTOFMEMORY;
        f->bgr24_buf = nb;
        f->bgr24_size = need;
    }

    int dst_stride = w * 3;
    for (int y = 0; y < h; y++) {
        const BYTE *s = bgr0 + (size_t)y * (size_t)stride_bytes;
        BYTE *d = f->bgr24_buf + (size_t)(h - 1 - y) * (size_t)dst_stride;
        for (int x = 0; x < w; x++) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            s += 4; d += 3;
        }
    }

    REFERENCE_TIME end = pts + (f->frame_dur_100ns > 0 ? f->frame_dur_100ns : 333333);
    HRESULT hr = ds_output_pin_deliver(f->pins[0], f->bgr24_buf, (long)need, pts, end);

    f->current_pos_100ns = pts;
    if (!f->first_frame_delivered) {
        f->first_frame_delivered = TRUE;
        SetEvent(f->first_frame_event);
        proxy_log("  First video frame delivered - renderer ready");
    }
    return hr;
}

/* ========== IEnumPins ========== */

typedef struct FilterEnumPins {
    IEnumPinsVtbl_DS *lpVtbl;
    LONG              ref_count;
    DSSourceFilter   *filter;
    int               index;
} FilterEnumPins;

static HRESULT STDMETHODCALLTYPE EP_QI(IEnumPins_DS *This, REFIID riid, void **ppv) {
    TRACE_MSG("EP_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumPins)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EP_AddRef(IEnumPins_DS *This) {
    return InterlockedIncrement(&((FilterEnumPins*)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE EP_Release(IEnumPins_DS *This) {
    TRACE_MSG("EP_Release");
    FilterEnumPins *e = (FilterEnumPins*)This;
    LONG ref = InterlockedDecrement(&e->ref_count);
    if (ref == 0) free(e);
    return ref;
}
static HRESULT STDMETHODCALLTYPE EP_Next(IEnumPins_DS *This, ULONG count, IPin_DS **out, ULONG *fetched) {
    TRACE_MSG("EP_Next");
    FilterEnumPins *e = (FilterEnumPins*)This;
    ULONG got = 0;
    while (got < count && e->index < e->filter->pin_count) {
        out[got] = (IPin_DS*)e->filter->pins[e->index];
        out[got]->lpVtbl->AddRef(out[got]);
        e->index++;
        got++;
    }
    if (fetched) *fetched = got;
    return (got == count) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EP_Skip(IEnumPins_DS *This, ULONG n) {
    TRACE_MSG("EP_Skip");
    ((FilterEnumPins*)This)->index += n; return S_OK;
}
static HRESULT STDMETHODCALLTYPE EP_Reset(IEnumPins_DS *This) {
    TRACE_MSG("EP_Reset");
    ((FilterEnumPins*)This)->index = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE EP_Clone(IEnumPins_DS *This, IEnumPins_DS **pp) {
    return E_NOTIMPL;
}

static IEnumPinsVtbl_DS g_EPVtbl = {
    EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone
};

/* ========== IBaseFilter ========== */

static HRESULT STDMETHODCALLTYPE BF_QI(IBaseFilter_DS *This, REFIID riid, void **ppv) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    proxy_log("DSFilter::QI({%08lX-%04X-%04X})", riid->Data1, riid->Data2, riid->Data3);
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IBaseFilter) ||
        IsEqualGUID(riid, &IID_IMediaFilter) || IsEqualGUID(riid, &IID_IPersist)) {
        *ppv = This; This->lpVtbl->AddRef(This);
        proxy_log("  -> IBaseFilter %p", *ppv);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IFileSourceFilter)) {
        *ppv = &f->lpFileSourceVtbl;
        InterlockedIncrement(&f->ref_count);
        proxy_log("  -> IFileSourceFilter %p", *ppv);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IMediaSeeking)) {
        *ppv = &f->lpMediaSeekingVtbl;
        InterlockedIncrement(&f->ref_count);
        proxy_log("  -> IMediaSeeking %p", *ppv);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IBasicAudio)) {
        *ppv = &f->lpBasicAudioVtbl;
        InterlockedIncrement(&f->ref_count);
        proxy_log("  -> IBasicAudio %p", *ppv);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IMediaPosition)) {
        *ppv = &f->lpMediaPositionVtbl;
        InterlockedIncrement(&f->ref_count);
        proxy_log("  -> IMediaPosition %p", *ppv);
        return S_OK;
    }
    proxy_log("  -> E_NOINTERFACE");
    *ppv = NULL; return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE BF_AddRef(IBaseFilter_DS *This) {
    TRACE_MSG("BF_AddRef");
    LONG ref = InterlockedIncrement(&((DSSourceFilter*)This)->ref_count);
    proxy_log("DSFilter::AddRef() -> %ld", ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE BF_Release(IBaseFilter_DS *This) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    LONG ref = InterlockedDecrement(&f->ref_count);
    if (ref == 0) {
        proxy_log("DSFilter destroyed");
        if (f->player) { mpv_player_destroy(f->player); f->player = NULL; }
        for (int i = 0; i < f->pin_count; i++)
            if (f->pins[i]) f->pins[i]->lpVtbl->Release((IPin_DS*)f->pins[i]);
        if (f->first_frame_event) CloseHandle(f->first_frame_event);
        DeleteCriticalSection(&f->cs);
        free(f->bgr24_buf);
        free(f->filename);
        free(f);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE BF_GetClassID(IBaseFilter_DS *This, CLSID *pClassID) {
    proxy_log("DSFilter::GetClassID()");
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_WMAsfReader;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_Stop(IBaseFilter_DS *This) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    proxy_log("DSFilter::Stop()");
    EnterCriticalSection(&f->cs);
    f->state = State_Stopped;
    /* Game cycles Stop+Run every frame; just pause mpv (idempotent) and
       leave allocators committed. */
    if (f->player) mpv_player_set_pause(f->player, TRUE);
    f->running = FALSE;
    LeaveCriticalSection(&f->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_Pause(IBaseFilter_DS *This) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    proxy_log("DSFilter::Pause()");
    EnterCriticalSection(&f->cs);
    if (f->state == State_Stopped) {
        for (int i = 0; i < f->pin_count; i++)
            if (f->pins[i] && f->pins[i]->allocator)
                f->pins[i]->allocator->lpVtbl->Commit(f->pins[i]->allocator);
    }
    if (f->player) mpv_player_set_pause(f->player, TRUE);
    f->state = State_Paused;
    LeaveCriticalSection(&f->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_Run(IBaseFilter_DS *This, REFERENCE_TIME tStart) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    proxy_log("DSFilter::Run(tStart=%lld)", (long long)tStart);
    EnterCriticalSection(&f->cs);
    if (f->state == State_Stopped) {
        for (int i = 0; i < f->pin_count; i++)
            if (f->pins[i] && f->pins[i]->allocator)
                f->pins[i]->allocator->lpVtbl->Commit(f->pins[i]->allocator);
    }
    f->start_time = tStart;
    f->state = State_Running;
    if (!f->running) {
        if (f->player) mpv_player_set_pause(f->player, FALSE);
        f->running = TRUE;
    }
    LeaveCriticalSection(&f->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_GetState(IBaseFilter_DS *This, DWORD ms, FILTER_STATE *pState) {
    TRACE_MSG("BF_GetState");
    if (!pState) return E_POINTER;
    *pState = ((DSSourceFilter*)This)->state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_SetSyncSource(IBaseFilter_DS *This, IReferenceClock_DS *pClock) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    if (f->clock) ((IUnknown*)f->clock)->lpVtbl->Release((IUnknown*)f->clock);
    f->clock = pClock;
    if (pClock) ((IUnknown*)pClock)->lpVtbl->AddRef((IUnknown*)pClock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_GetSyncSource(IBaseFilter_DS *This, IReferenceClock_DS **ppClock) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    if (!ppClock) return E_POINTER;
    *ppClock = f->clock;
    if (f->clock) ((IUnknown*)f->clock)->lpVtbl->AddRef((IUnknown*)f->clock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_EnumPins(IBaseFilter_DS *This, IEnumPins_DS **ppEnum) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    if (!ppEnum) return E_POINTER;
    FilterEnumPins *e = calloc(1, sizeof(*e));
    if (!e) return E_OUTOFMEMORY;
    e->lpVtbl = &g_EPVtbl;
    e->ref_count = 1;
    e->filter = f;
    *ppEnum = (IEnumPins_DS*)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_FindPin(IBaseFilter_DS *This, LPCWSTR Id, IPin_DS **ppPin) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    if (!ppPin) return E_POINTER;
    for (int i = 0; i < f->pin_count; i++) {
        if (wcscmp(f->pins[i]->pin_id, Id) == 0) {
            *ppPin = (IPin_DS*)f->pins[i];
            (*ppPin)->lpVtbl->AddRef(*ppPin);
            return S_OK;
        }
    }
    return VFW_E_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE BF_QueryFilterInfo(IBaseFilter_DS *This, FILTER_INFO *pInfo) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    if (!pInfo) return E_POINTER;
    wcscpy(pInfo->achName, f->filter_name);
    pInfo->pGraph = f->graph;
    if (f->graph) ((IUnknown*)f->graph)->lpVtbl->AddRef((IUnknown*)f->graph);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_JoinFilterGraph(IBaseFilter_DS *This, IFilterGraph_DS *pGraph, LPCWSTR pName) {
    DSSourceFilter *f = (DSSourceFilter*)This;
    f->graph = pGraph;
    if (pName) wcsncpy(f->filter_name, pName, 127);
    proxy_log("DSFilter::JoinFilterGraph(name=%ls)", pName ? pName : L"(null)");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE BF_QueryVendorInfo(IBaseFilter_DS *This, LPWSTR *pVendorInfo) {
    return E_NOTIMPL;
}

static IBaseFilterVtbl_DS g_BFVtbl = {
    BF_QI, BF_AddRef, BF_Release,
    BF_GetClassID,
    BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource,
    BF_EnumPins, BF_FindPin, BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo
};

/* ========== IFileSourceFilter ========== */

static HRESULT STDMETHODCALLTYPE FS_QI(IFileSourceFilter_DS *This, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)FILTER_FROM_FILESOURCE(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE FS_AddRef(IFileSourceFilter_DS *This) {
    return InterlockedIncrement(&FILTER_FROM_FILESOURCE(This)->ref_count);
}
static ULONG STDMETHODCALLTYPE FS_Release(IFileSourceFilter_DS *This) {
    return BF_Release((IBaseFilter_DS*)FILTER_FROM_FILESOURCE(This));
}

static HRESULT STDMETHODCALLTYPE FS_Load(IFileSourceFilter_DS *This, LPCOLESTR pszFileName, const AM_MEDIA_TYPE *pmt) {
    DSSourceFilter *f = FILTER_FROM_FILESOURCE(This);
    proxy_log("DSFilter::Load(\"%ls\")", pszFileName);

    /* Convert path to UTF-8 for libmpv. */
    int len = WideCharToMultiByte(CP_UTF8, 0, pszFileName, -1, NULL, 0, NULL, NULL);
    char *utf8 = malloc(len);
    if (!utf8) return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_UTF8, 0, pszFileName, -1, utf8, len, NULL, NULL);

    f->player = mpv_player_create();
    if (!f->player) { free(utf8); return E_FAIL; }

    /* Volume that game already set before Load (default 0 = full). */
    mpv_player_set_volume_centibels(f->player, f->audio_volume);

    /* Only loop the menu background. Intros must reach EOF naturally so the
       game's "is finished" check progresses past them. Heuristic: filename
       contains "loop" (frontend_loop.wmv etc.). */
    BOOL should_loop = FALSE;
    {
        WCHAR lc[MAX_PATH];
        size_t n = wcslen(pszFileName);
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        for (size_t i = 0; i < n; i++) lc[i] = (WCHAR)towlower(pszFileName[i]);
        lc[n] = 0;
        if (wcsstr(lc, L"loop")) should_loop = TRUE;
    }
    proxy_log("  loop=%s", should_loop ? "yes" : "no");

    int w = 0, h = 0;
    double fps = 30.0, dur_sec = 0.0;
    HRESULT hr = mpv_player_load(f->player, utf8, should_loop, &w, &h, &fps, &dur_sec);
    free(utf8);
    if (FAILED(hr)) {
        mpv_player_destroy(f->player); f->player = NULL;
        return hr;
    }

    f->duration_100ns = (LONGLONG)(dur_sec * 10000000.0);
    f->frame_dur_100ns = (fps > 0.0)
        ? (REFERENCE_TIME)(10000000.0 / fps)
        : (REFERENCE_TIME)333333;

    /* Single video pin. libmpv handles audio internally via WASAPI. */
    DSOutputPin *pin = ds_output_pin_create(f, w, h);
    if (!pin) {
        mpv_player_destroy(f->player); f->player = NULL;
        return E_FAIL;
    }
    f->pins[f->pin_count++] = pin;
    proxy_log("  video pin[0] %dx%d @ %.3f fps", w, h, fps);

    /* Now wire the frame callback so subsequent frames flow to the renderer. */
    mpv_player_set_frame_callback(f->player, on_mpv_frame, f);

    /* Store filename for IFileSourceFilter::GetCurFile. */
    size_t fn_len = (wcslen(pszFileName) + 1) * sizeof(WCHAR);
    f->filename = malloc(fn_len);
    if (f->filename) memcpy(f->filename, pszFileName, fn_len);

    proxy_log("  loaded: dur=%.3fs", dur_sec);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FS_GetCurFile(IFileSourceFilter_DS *This, LPOLESTR *ppszFileName, AM_MEDIA_TYPE *pmt) {
    DSSourceFilter *f = FILTER_FROM_FILESOURCE(This);
    if (ppszFileName) {
        if (f->filename) {
            size_t len = (wcslen(f->filename) + 1) * sizeof(WCHAR);
            *ppszFileName = (LPOLESTR)CoTaskMemAlloc(len);
            if (*ppszFileName) memcpy(*ppszFileName, f->filename, len);
        } else {
            *ppszFileName = NULL;
        }
    }
    if (pmt) memset(pmt, 0, sizeof(*pmt));
    return S_OK;
}

static IFileSourceFilterVtbl_DS g_FSVtbl = {
    FS_QI, FS_AddRef, FS_Release, FS_Load, FS_GetCurFile
};

/* ========== IMediaSeeking ========== */

static HRESULT STDMETHODCALLTYPE MS_QI(IMediaSeeking_DS *This, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)FILTER_FROM_SEEKING(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE MS_AddRef(IMediaSeeking_DS *This) {
    return InterlockedIncrement(&FILTER_FROM_SEEKING(This)->ref_count);
}
static ULONG STDMETHODCALLTYPE MS_Release(IMediaSeeking_DS *This) {
    return BF_Release((IBaseFilter_DS*)FILTER_FROM_SEEKING(This));
}

#define MS_CAPS (AM_SEEKING_CanSeekAbsolute | AM_SEEKING_CanSeekForwards | \
                 AM_SEEKING_CanSeekBackwards | AM_SEEKING_CanGetCurrentPos | \
                 AM_SEEKING_CanGetDuration | AM_SEEKING_CanGetStopPos)

static HRESULT STDMETHODCALLTYPE MS_GetCapabilities(IMediaSeeking_DS *This, DWORD *p) {
    proxy_log("DSFilter::MS_GetCapabilities()");
    if (p) *p = MS_CAPS; return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_CheckCapabilities(IMediaSeeking_DS *This, DWORD *p) {
    TRACE_MSG("MS_CheckCapabilities");
    if (!p) return E_POINTER;
    DWORD ask = *p; *p = ask & MS_CAPS;
    return (*p == ask) ? S_OK : ((*p) ? S_FALSE : E_FAIL);
}
static HRESULT STDMETHODCALLTYPE MS_IsFormatSupported(IMediaSeeking_DS *This, const GUID *pFormat) {
    TRACE_MSG("MS_IsFormatSupported(p=%p)", pFormat);
    if (!pFormat) return S_FALSE;
    return IsEqualGUID(pFormat, &TIME_FORMAT_MEDIA_TIME) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE MS_QueryPreferredFormat(IMediaSeeking_DS *This, GUID *p) {
    TRACE_MSG("MS_QueryPreferredFormat");
    if (p) *p = TIME_FORMAT_MEDIA_TIME; return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_GetTimeFormat(IMediaSeeking_DS *This, GUID *p) {
    TRACE_MSG("MS_GetTimeFormat");
    if (p) *p = TIME_FORMAT_MEDIA_TIME; return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_IsUsingTimeFormat(IMediaSeeking_DS *This, const GUID *p) {
    TRACE_MSG("MS_IsUsingTimeFormat(p=%p)", p);
    if (!p) return S_OK;
    return IsEqualGUID(p, &TIME_FORMAT_MEDIA_TIME) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE MS_SetTimeFormat(IMediaSeeking_DS *This, const GUID *p) {
    TRACE_MSG("MS_SetTimeFormat(p=%p)", p);
    if (!p) return S_OK;
    return IsEqualGUID(p, &TIME_FORMAT_MEDIA_TIME) ? S_OK : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE MS_GetDuration(IMediaSeeking_DS *This, LONGLONG *p) {
    proxy_log("DSFilter::MS_GetDuration()");
    if (!p) return E_POINTER;
    *p = FILTER_FROM_SEEKING(This)->duration_100ns;
    proxy_log("  -> %lld", (long long)*p);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_GetStopPosition(IMediaSeeking_DS *This, LONGLONG *p) {
    TRACE_MSG("MS_GetStopPosition");
    if (!p) return E_POINTER;
    *p = FILTER_FROM_SEEKING(This)->duration_100ns;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_GetCurrentPosition(IMediaSeeking_DS *This, LONGLONG *p) {
    TRACE_MSG("MS_GetCurrentPosition");
    if (!p) return E_POINTER;
    DSSourceFilter *f = FILTER_FROM_SEEKING(This);
    if (f->player) {
        double pos = mpv_player_get_position(f->player);
        *p = (LONGLONG)(pos * 10000000.0);
    } else {
        *p = f->current_pos_100ns;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_ConvertTimeFormat(IMediaSeeking_DS *This, LONGLONG *t, const GUID *tf, LONGLONG s, const GUID *sf) {
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE MS_SetPositions(IMediaSeeking_DS *This,
    LONGLONG *pCurrent, DWORD dwCurrentFlags, LONGLONG *pStop, DWORD dwStopFlags)
{
    /* libmpv handles seek via property; the game rarely seeks short intros.
       Stub for now — extend if a real consumer path needs it. */
    if (pCurrent && (dwCurrentFlags & 0x1))
        proxy_log("DSFilter::SetPositions(pos=%lld) — ignored", (long long)*pCurrent);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE MS_GetPositions(IMediaSeeking_DS *This, LONGLONG *pCurrent, LONGLONG *pStop) {
    DSSourceFilter *f = FILTER_FROM_SEEKING(This);
    if (pCurrent) MS_GetCurrentPosition(This, pCurrent);
    if (pStop) *pStop = f->duration_100ns;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE MS_GetAvailable(IMediaSeeking_DS *This, LONGLONG *pEarliest, LONGLONG *pLatest) {
    DSSourceFilter *f = FILTER_FROM_SEEKING(This);
    if (pEarliest) *pEarliest = 0;
    if (pLatest) *pLatest = f->duration_100ns;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MS_SetRate(IMediaSeeking_DS *This, double r) { return S_OK; }
static HRESULT STDMETHODCALLTYPE MS_GetRate(IMediaSeeking_DS *This, double *p) { if (p) *p = 1.0; return S_OK; }
static HRESULT STDMETHODCALLTYPE MS_GetPreroll(IMediaSeeking_DS *This, LONGLONG *p) { if (p) *p = 0; return S_OK; }

static IMediaSeekingVtbl_DS g_MSVtbl = {
    MS_QI, MS_AddRef, MS_Release,
    MS_GetCapabilities, MS_CheckCapabilities,
    MS_IsFormatSupported, MS_QueryPreferredFormat,
    MS_GetTimeFormat, MS_IsUsingTimeFormat, MS_SetTimeFormat,
    MS_GetDuration, MS_GetStopPosition, MS_GetCurrentPosition,
    MS_ConvertTimeFormat, MS_SetPositions, MS_GetPositions,
    MS_GetAvailable, MS_SetRate, MS_GetRate, MS_GetPreroll
};

/* ========== IBasicAudio ========== */

static HRESULT STDMETHODCALLTYPE BA_QI(IBasicAudio_DS *This, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)FILTER_FROM_BASICAUDIO(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE BA_AddRef(IBasicAudio_DS *This) {
    return InterlockedIncrement(&FILTER_FROM_BASICAUDIO(This)->ref_count);
}
static ULONG STDMETHODCALLTYPE BA_Release(IBasicAudio_DS *This) {
    return BF_Release((IBaseFilter_DS*)FILTER_FROM_BASICAUDIO(This));
}
static HRESULT STDMETHODCALLTYPE BA_GetTypeInfoCount(IBasicAudio_DS *This, UINT *p) {
    TRACE_MSG("BA_GetTypeInfoCount");
    if (p) *p = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE BA_GetTypeInfo(IBasicAudio_DS *This, UINT i, LCID l, void **pp) {
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE BA_GetIDsOfNames(IBasicAudio_DS *This, REFIID r, LPOLESTR *n, UINT c, LCID l, DISPID *d) {
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE BA_Invoke(IBasicAudio_DS *This, DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *v, EXCEPINFO *e, UINT *a) {
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE BA_put_Volume(IBasicAudio_DS *This, long vol) {
    DSSourceFilter *f = FILTER_FROM_BASICAUDIO(This);
    proxy_log("IBasicAudio::put_Volume(%ld)", vol);
    f->audio_volume = vol;
    if (f->player) mpv_player_set_volume_centibels(f->player, vol);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BA_get_Volume(IBasicAudio_DS *This, long *vol) {
    TRACE_MSG("BA_get_Volume");
    if (vol) *vol = FILTER_FROM_BASICAUDIO(This)->audio_volume;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BA_put_Balance(IBasicAudio_DS *This, long bal) {
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BA_get_Balance(IBasicAudio_DS *This, long *bal) {
    TRACE_MSG("BA_get_Balance");
    if (bal) *bal = 0;
    return S_OK;
}

static IBasicAudioVtbl_DS g_BAVtbl = {
    BA_QI, BA_AddRef, BA_Release,
    BA_GetTypeInfoCount, BA_GetTypeInfo, BA_GetIDsOfNames, BA_Invoke,
    BA_put_Volume, BA_get_Volume, BA_put_Balance, BA_get_Balance
};

/* ========== IMediaPosition ========== */

static HRESULT STDMETHODCALLTYPE MP_QI(void *This, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)FILTER_FROM_MEDIAPOS(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE MP_AddRef(void *This) {
    return InterlockedIncrement(&FILTER_FROM_MEDIAPOS(This)->ref_count);
}
static ULONG STDMETHODCALLTYPE MP_Release(void *This) {
    return BF_Release((IBaseFilter_DS*)FILTER_FROM_MEDIAPOS(This));
}
static HRESULT STDMETHODCALLTYPE MP_GetTypeInfoCount(void *This, UINT *p) { if (p) *p = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_GetTypeInfo(void *This, UINT i, LCID l, void **pp) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE MP_GetIDsOfNames(void *This, REFIID r, LPOLESTR *n, UINT c, LCID l, DISPID *d) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE MP_Invoke(void *This, DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *v, EXCEPINFO *e, UINT *a) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE MP_get_Duration(void *This, double *pDuration) {
    DSSourceFilter *f = FILTER_FROM_MEDIAPOS(This);
    if (!pDuration) return E_POINTER;
    *pDuration = (double)f->duration_100ns / 10000000.0;
    proxy_log("IMediaPosition::get_Duration() -> %.3f", *pDuration);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MP_put_CurrentPosition(void *This, double pos) {
    proxy_log("IMediaPosition::put_CurrentPosition(%.3f)", pos);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MP_get_CurrentPosition(void *This, double *pPos) {
    DSSourceFilter *f = FILTER_FROM_MEDIAPOS(This);
    if (!pPos) return E_POINTER;
    if (f->player) *pPos = mpv_player_get_position(f->player);
    else           *pPos = (double)f->current_pos_100ns / 10000000.0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MP_get_StopTime(void *This, double *p) {
    DSSourceFilter *f = FILTER_FROM_MEDIAPOS(This);
    if (p) *p = (double)f->duration_100ns / 10000000.0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MP_put_StopTime(void *This, double v) { return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_get_PrerollTime(void *This, double *p) { if (p) *p = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_put_PrerollTime(void *This, double v) { return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_put_Rate(void *This, double r) { return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_get_Rate(void *This, double *p) { if (p) *p = 1.0; return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_CanSeekForward(void *This, long *p) { if (p) *p = -1; return S_OK; }
static HRESULT STDMETHODCALLTYPE MP_CanSeekBackward(void *This, long *p) { if (p) *p = -1; return S_OK; }

static IMediaPositionVtbl_DS g_MPVtbl = {
    MP_QI, MP_AddRef, MP_Release,
    MP_GetTypeInfoCount, MP_GetTypeInfo, MP_GetIDsOfNames, MP_Invoke,
    MP_get_Duration, MP_put_CurrentPosition, MP_get_CurrentPosition,
    MP_get_StopTime, MP_put_StopTime,
    MP_get_PrerollTime, MP_put_PrerollTime,
    MP_put_Rate, MP_get_Rate,
    MP_CanSeekForward, MP_CanSeekBackward
};

/* ========== Factory ========== */

HRESULT ds_source_filter_create(IBaseFilter_DS **ppFilter) {
    if (!ppFilter) return E_POINTER;

    DSSourceFilter *f = calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;

    f->lpVtbl = &g_BFVtbl;
    f->lpFileSourceVtbl = &g_FSVtbl;
    f->lpMediaSeekingVtbl = &g_MSVtbl;
    f->lpBasicAudioVtbl = &g_BAVtbl;
    f->lpMediaPositionVtbl = &g_MPVtbl;
    f->ref_count = 1;
    f->state = State_Stopped;
    f->frame_dur_100ns = 333333; /* 30fps default until Load is called */

    InitializeCriticalSection(&f->cs);
    f->first_frame_event = CreateEventA(NULL, TRUE, FALSE, NULL);

    *ppFilter = (IBaseFilter_DS*)f;
    proxy_log("Created DSSourceFilter @ %p (libmpv backend)", f);
    return S_OK;
}

IBasicAudio_DS *ds_source_filter_get_basic_audio(IBaseFilter_DS *pFilter) {
    DSSourceFilter *f = (DSSourceFilter*)pFilter;
    InterlockedIncrement(&f->ref_count);
    return (IBasicAudio_DS*)&f->lpBasicAudioVtbl;
}

void *ds_source_filter_get_media_position(IBaseFilter_DS *pFilter) {
    DSSourceFilter *f = (DSSourceFilter*)pFilter;
    InterlockedIncrement(&f->ref_count);
    return &f->lpMediaPositionVtbl;
}

BOOL ds_source_filter_wait_first_frame(IBaseFilter_DS *pFilter, DWORD timeout_ms) {
    DSSourceFilter *f = (DSSourceFilter*)pFilter;
    if (!f->first_frame_event) return FALSE;
    proxy_log("Waiting for first video frame (timeout=%lums)", timeout_ms);
    DWORD result = WaitForSingleObject(f->first_frame_event, timeout_ms);
    proxy_log("  wait result: %s", result == WAIT_OBJECT_0 ? "READY" : "TIMEOUT");
    return (result == WAIT_OBJECT_0);
}
