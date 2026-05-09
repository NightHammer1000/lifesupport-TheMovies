/*
 * Fake DirectShow FilterGraph — replaces quartz.dll entirely.
 *
 * The game expects: IGraphBuilder, IMediaControl, IMediaSeeking,
 * IMediaPosition, IMediaEvent, IBasicAudio from the graph.
 * We implement all of them, delegating playback to our DSSourceFilter.
 */
#include "ds_fakegraph.h"
#include "ds_filter.h"
#include "ds_output_pin.h"
#include "trace.h"
#include "log.h"
#include <mmsystem.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ========== FakeGraph struct ========== */

/* IMediaControl vtable (IDispatch + Run/Pause/Stop/GetState/RenderFile/AddSourceFilter) */
typedef struct IMediaControlVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(void*, UINT*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(void*, UINT, LCID, void**);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(void*, REFIID, LPOLESTR*, UINT, LCID, DISPID*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(void*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*);
    HRESULT (STDMETHODCALLTYPE *Run)(void*);
    HRESULT (STDMETHODCALLTYPE *Pause)(void*);
    HRESULT (STDMETHODCALLTYPE *Stop)(void*);
    HRESULT (STDMETHODCALLTYPE *GetState)(void*, long, long*);
    HRESULT (STDMETHODCALLTYPE *RenderFile)(void*, BSTR);
    HRESULT (STDMETHODCALLTYPE *AddSourceFilter)(void*, BSTR, void**);
    HRESULT (STDMETHODCALLTYPE *get_FilterCollection)(void*, void**);
    HRESULT (STDMETHODCALLTYPE *get_RegFilterCollection)(void*, void**);
    HRESULT (STDMETHODCALLTYPE *StopWhenReady)(void*);
} IMediaControlVtbl_DS;

/* IMediaEvent vtable (IDispatch + events) */
typedef struct IMediaEventVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(void*, UINT*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(void*, UINT, LCID, void**);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(void*, REFIID, LPOLESTR*, UINT, LCID, DISPID*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(void*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*);
    HRESULT (STDMETHODCALLTYPE *GetEventHandle)(void*, LONG_PTR*);
    HRESULT (STDMETHODCALLTYPE *GetEvent)(void*, long*, LONG_PTR*, LONG_PTR*, long);
    HRESULT (STDMETHODCALLTYPE *WaitForCompletion)(void*, long, long*);
    HRESULT (STDMETHODCALLTYPE *CancelDefaultHandling)(void*, long);
    HRESULT (STDMETHODCALLTYPE *RestoreDefaultHandling)(void*, long);
    HRESULT (STDMETHODCALLTYPE *FreeEventParams)(void*, long, LONG_PTR, LONG_PTR);
} IMediaEventVtbl_DS;

#define MAX_GRAPH_FILTERS 8

typedef struct FakeGraph {
    /* Interface vtable pointers — each at a known offset for QI */
    IBaseFilterVtbl_DS      *lpGraphBuilderVtbl;   /* 0: IGraphBuilder (reuses IBaseFilter layout for QI/AddRef/Release + graph methods) */
    IMediaControlVtbl_DS    *lpMediaControlVtbl;   /* 4 */
    IMediaEventVtbl_DS      *lpMediaEventVtbl;     /* 8 */
    IMediaSeekingVtbl_DS    *lpMediaSeekingVtbl;   /* 12 */
    void                    *lpMediaPositionVtbl;  /* 16 — IMediaPositionVtbl_FG* */
    IBasicAudioVtbl_DS      *lpBasicAudioVtbl;     /* 20 */
    LONG                     ref_count;             /* 24 */

    /* Filters added to this graph */
    IBaseFilter_DS          *filters[MAX_GRAPH_FILTERS];
    int                      filter_count;

    /* Our source filter (set when WMAsfReader is added) */
    IBaseFilter_DS          *source_filter;

    /* The game's texture renderer (set when TEXTURERENDERER is added) */
    IBaseFilter_DS          *video_renderer;
    IPin_DS                 *video_renderer_pin;

    /* Event for completion */
    HANDLE                   complete_event;

    /* Audio volume (for IBasicAudio) */
    long                     volume;
    BOOL                     running;
} FakeGraph;

/* Offset macros */
#define GRAPH_FROM_MC(p)  ((FakeGraph*)((BYTE*)(p) - offsetof(FakeGraph, lpMediaControlVtbl)))
#define GRAPH_FROM_ME(p)  ((FakeGraph*)((BYTE*)(p) - offsetof(FakeGraph, lpMediaEventVtbl)))
#define GRAPH_FROM_MS(p)  ((FakeGraph*)((BYTE*)(p) - offsetof(FakeGraph, lpMediaSeekingVtbl)))
#define GRAPH_FROM_MP(p)  ((FakeGraph*)((BYTE*)(p) - offsetof(FakeGraph, lpMediaPositionVtbl)))
#define GRAPH_FROM_BA(p)  ((FakeGraph*)((BYTE*)(p) - offsetof(FakeGraph, lpBasicAudioVtbl)))

/* ========== Forward decls ========== */
static IMediaControlVtbl_DS g_FGMediaControlVtbl;
static IMediaEventVtbl_DS g_FGMediaEventVtbl;
static IMediaSeekingVtbl_DS g_FGMediaSeekingVtbl;
static IBasicAudioVtbl_DS g_FGBasicAudioVtbl;

/* IMediaPosition vtable — same type as in ds_filter.c */
typedef struct IMediaPositionVtbl_FG {
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
} IMediaPositionVtbl_FG;
static IMediaPositionVtbl_FG g_FGMediaPositionVtbl;

/* IID for IMediaControl: {56A868B1-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMediaControl =
    {0x56A868B1, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* ========== IGraphBuilder (QI, AddRef, Release + graph methods) ========== */

static HRESULT STDMETHODCALLTYPE FG_QI(IBaseFilter_DS *This, REFIID riid, void **ppv) {
    FakeGraph *g = (FakeGraph*)This;
    TRACE_MSG("FG::QI({%08lX-%04X})", riid->Data1, riid->Data2);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IGraphBuilder)) {
        *ppv = This; goto ok;
    }
    if (IsEqualGUID(riid, &IID_IMediaControl)) {
        *ppv = &g->lpMediaControlVtbl; goto ok;
    }
    if (IsEqualGUID(riid, &IID_IMediaEvent)) {
        *ppv = &g->lpMediaEventVtbl; goto ok;
    }
    if (IsEqualGUID(riid, &IID_IMediaSeeking)) {
        /* Game stores IMediaSeeking but calls IMediaPosition methods through it.
           Return IMediaPosition so vtable layout and __stdcall cleanup sizes match. */
        *ppv = &g->lpMediaPositionVtbl; goto ok;
    }
    if (IsEqualGUID(riid, &IID_IMediaPosition)) {
        *ppv = &g->lpMediaPositionVtbl; goto ok;
    }
    if (IsEqualGUID(riid, &IID_IBasicAudio)) {
        *ppv = &g->lpBasicAudioVtbl; goto ok;
    }
    /* Also accept IFilterGraph, IFilterGraph2 */
    if (riid->Data1 == 0x56A8689F || riid->Data1 == 0x56A868A2) {
        *ppv = This; goto ok;
    }

    proxy_log("  -> E_NOINTERFACE");
    *ppv = NULL;
    return E_NOINTERFACE;
ok:
    InterlockedIncrement(&g->ref_count);
    proxy_log("  -> %p (ref=%ld)", *ppv, g->ref_count);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE FG_AddRef(IBaseFilter_DS *This) {
    return InterlockedIncrement(&((FakeGraph*)This)->ref_count);
}

static ULONG STDMETHODCALLTYPE FG_Release(IBaseFilter_DS *This) {
    FakeGraph *g = (FakeGraph*)This;
    LONG ref = InterlockedDecrement(&g->ref_count);
    proxy_log("FakeGraph::Release() -> ref=%ld", ref);
    if (ref == 0) {
        proxy_log("FakeGraph destroyed");
        if (g->complete_event) CloseHandle(g->complete_event);
        for (int i = 0; i < g->filter_count; i++)
            if (g->filters[i]) g->filters[i]->lpVtbl->Release(g->filters[i]);
        free(g);
    }
    return ref;
}

/* IMediaEventSink::Notify — the TEXTURERENDERER calls this on EndOfStream.
   Because IMediaEventSink[3] maps to IFilterGraph[3] (AddFilter), the renderer's
   Notify(EC_COMPLETE, 0, 0) hits AddFilter with pFilter=1. We detect this and handle it. */
static HRESULT STDMETHODCALLTYPE FG_EventSinkNotify(IBaseFilter_DS *This, long EventCode, LONG_PTR p1, LONG_PTR p2) {
    FakeGraph *g = (FakeGraph*)This;
    proxy_log("FakeGraph::EventSinkNotify(code=%ld, p1=%ld, p2=%ld)", EventCode, p1, p2);
    if (EventCode == EC_COMPLETE && g->complete_event) {
        SetEvent(g->complete_event);
    }
    return S_OK;
}

/* IGraphBuilder methods */
static HRESULT STDMETHODCALLTYPE FG_AddFilter(IBaseFilter_DS *This, IBaseFilter_DS *pFilter, LPCWSTR pName) {
    FakeGraph *g = (FakeGraph*)This;

    /* Detect IMediaEventSink::Notify disguised as AddFilter.
       The TEXTURERENDERER calls Notify(EC_COMPLETE=1, 0, 0) through the IFilterGraph pointer.
       IMediaEventSink vtable slot [3] = Notify maps to IFilterGraph slot [3] = AddFilter. */
    if ((ULONG_PTR)pFilter < 0x10000) {
        return FG_EventSinkNotify(This, (long)(LONG_PTR)pFilter, (LONG_PTR)pName, 0);
    }

    proxy_log("FakeGraph::AddFilter(%p, \"%ls\")", pFilter, pName ? pName : L"(null)");

    if (g->filter_count < MAX_GRAPH_FILTERS) {
        pFilter->lpVtbl->AddRef(pFilter);
        g->filters[g->filter_count++] = pFilter;
    }

    /* Detect our source filter vs the game's texture renderer */
    CLSID clsid;
    if (pFilter->lpVtbl->GetClassID(pFilter, &clsid) == S_OK) {
        if (IsEqualGUID(&clsid, &CLSID_WMAsfReader)) {
            g->source_filter = pFilter;
            proxy_log("  -> identified as source filter");
        }
    }
    if (pName && wcscmp(pName, L"TEXTURERENDERER") == 0) {
        g->video_renderer = pFilter;
        proxy_log("  -> identified as video renderer");
    }

    /* Tell the filter it joined this graph */
    pFilter->lpVtbl->JoinFilterGraph(pFilter, (IFilterGraph_DS*)g, pName);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FG_Render(IBaseFilter_DS *This, IPin_DS *pPin) {
    FakeGraph *g = (FakeGraph*)This;
    proxy_log("FakeGraph::Render(pin=%p) filters=%d", pPin, g->filter_count);

    /* If we haven't identified the video renderer yet, search all filters */
    if (!g->video_renderer) {
        for (int i = 0; i < g->filter_count; i++) {
            if (g->filters[i] != g->source_filter) {
                g->video_renderer = g->filters[i];
                proxy_log("  auto-detected video renderer: filter[%d]=%p", i, g->filters[i]);
                break;
            }
        }
    }

    if (!g->video_renderer) {
        proxy_log("  no video renderer to connect to");
        return S_OK;
    }

    /* Find the renderer's input pin */
    IEnumPins_DS *pEnum = NULL;
    g->video_renderer->lpVtbl->EnumPins(g->video_renderer, &pEnum);
    if (pEnum) {
        IPin_DS *rendererPin = NULL;
        ULONG fetched = 0;
        if (pEnum->lpVtbl->Next(pEnum, 1, &rendererPin, &fetched) == S_OK && rendererPin) {
            proxy_log("  connecting source pin to renderer pin %p", rendererPin);
            HRESULT hr = pPin->lpVtbl->Connect(pPin, rendererPin, NULL);
            proxy_log("  Connect result: 0x%08lX", hr);
            if (hr == S_OK)
                g->video_renderer_pin = rendererPin;
            else
                rendererPin->lpVtbl->Release(rendererPin);
        }
        pEnum->lpVtbl->Release(pEnum);
    }
    return S_OK;
}

/* Stubs for unused IGraphBuilder methods */
static HRESULT STDMETHODCALLTYPE FG_RemoveFilter(void *t, void *f) { proxy_log("FakeGraph::RemoveFilter()"); return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_EnumFilters(void *t, void **pp) { proxy_log("FakeGraph::EnumFilters()"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_FindFilterByName(void *t, LPCWSTR n, void **pp) { proxy_log("FakeGraph::FindFilterByName(%ls)", n?n:L"null"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_ConnectDirect(void *t, void *a, void *b, void *c) { proxy_log("FakeGraph::ConnectDirect()"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_Reconnect(void *t, void *p) { proxy_log("FakeGraph::Reconnect()"); return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_Disconnect(void *t, void *p) { proxy_log("FakeGraph::Disconnect()"); return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_SetDefaultSyncSource(void *t) { proxy_log("FakeGraph::SetDefaultSyncSource()"); return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_Connect(void *t, void *a, void *b) { proxy_log("FakeGraph::Connect()"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_RenderFile(void *t, LPCWSTR f, LPCWSTR r) { proxy_log("FakeGraph::RenderFile(%ls)", f?f:L"null"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_AddSourceFilter(void *t, LPCWSTR f, LPCWSTR n, void **pp) { proxy_log("FakeGraph::AddSourceFilter()"); return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE FG_SetLogFile(void *t, DWORD_PTR h) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_Abort(void *t) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FG_ShouldOperationContinue(void *t) { return S_OK; }

static void *g_FGGraphVtblRaw[18]; /* filled in ds_fakegraph_create */

/* ========== IMediaControl ========== */

static HRESULT STDMETHODCALLTYPE MC_QI(void *t, REFIID r, void **p) { return FG_QI((IBaseFilter_DS*)GRAPH_FROM_MC(t),r,p); }
static ULONG STDMETHODCALLTYPE MC_AddRef(void *t) { return InterlockedIncrement(&GRAPH_FROM_MC(t)->ref_count); }
static ULONG STDMETHODCALLTYPE MC_Release(void *t) { return FG_Release((IBaseFilter_DS*)GRAPH_FROM_MC(t)); }
static HRESULT STDMETHODCALLTYPE Disp_GetTypeInfoCount(void *t, UINT *p) { if(p)*p=0; return S_OK; }
static HRESULT STDMETHODCALLTYPE Disp_GetTypeInfo(void *t, UINT i, LCID l, void **pp) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE Disp_GetIDsOfNames(void *t, REFIID r, LPOLESTR *n, UINT c, LCID l, DISPID *d) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE Disp_Invoke(void *t, DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *v, EXCEPINFO *e, UINT *a) { return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE MC_Run(void *This) {
    FakeGraph *g = GRAPH_FROM_MC(This);
    /* Skip if already running — game calls this every frame */
    if (g->running) return S_OK;
    TRACE_MSG("FG::MC::Run() src=%p renderer=%p", g->source_filter, g->video_renderer);
    for (int i = 0; i < g->filter_count; i++) {
        if (g->filters[i]) {
            TRACE_MSG("  Pause filter[%d]=%p", i, g->filters[i]);
            g->filters[i]->lpVtbl->Pause(g->filters[i]);
        }
    }
    for (int i = 0; i < g->filter_count; i++) {
        if (g->filters[i]) {
            TRACE_MSG("  Run filter[%d]=%p", i, g->filters[i]);
            g->filters[i]->lpVtbl->Run(g->filters[i], 0);
        }
    }
    if (g->source_filter) {
        ds_source_filter_wait_first_frame(g->source_filter, 5000);
    }
    g->running = TRUE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MC_Pause(void *This) {
    FakeGraph *g = GRAPH_FROM_MC(This);
    TRACE_MSG("FG::MC::Pause()");
    for (int i = 0; i < g->filter_count; i++)
        if (g->filters[i]) g->filters[i]->lpVtbl->Pause(g->filters[i]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MC_Stop(void *This) {
    FakeGraph *g = GRAPH_FROM_MC(This);
    TRACE_MSG("FG::MC::Stop()");
    /* Don't actually stop filters here — the game's callback calls Stop+Run every frame.
       Real cleanup happens in FG_Release when the graph is destroyed. */
    g->running = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MC_GetState(void *This, long ms, long *pState) {
    FakeGraph *g = GRAPH_FROM_MC(This);
    TRACE_MSG("FG::MC::GetState()");
    /* Dump COM interface state for crash diagnosis */
    proxy_log("  FakeGraph @ %p ref=%ld", g, g->ref_count);
    proxy_log("  lpGraphBuilderVtbl=%p lpMediaControlVtbl=%p", g->lpGraphBuilderVtbl, g->lpMediaControlVtbl);
    proxy_log("  lpMediaEventVtbl=%p lpMediaSeekingVtbl=%p", g->lpMediaEventVtbl, g->lpMediaSeekingVtbl);
    proxy_log("  lpMediaPositionVtbl=%p lpBasicAudioVtbl=%p", g->lpMediaPositionVtbl, g->lpBasicAudioVtbl);
    /* Dump vtable entries for IMediaSeeking */
    if (g->lpMediaSeekingVtbl) {
        IMediaSeekingVtbl_DS *ms_vtbl = g->lpMediaSeekingVtbl;
        proxy_log("  IMediaSeeking vtbl: [0]=%p [3]=%p [8]=%p [10]=%p",
                  ms_vtbl->QueryInterface, ms_vtbl->GetCapabilities,
                  ms_vtbl->IsUsingTimeFormat, ms_vtbl->GetDuration);
    }
    /* Dump IMediaControl vtable entries */
    if (g->lpMediaControlVtbl) {
        IMediaControlVtbl_DS *mc_vtbl = g->lpMediaControlVtbl;
        proxy_log("  IMediaControl vtbl: Run=%p Pause=%p Stop=%p GetState=%p",
                  mc_vtbl->Run, mc_vtbl->Pause, mc_vtbl->Stop, mc_vtbl->GetState);
    }
    if (pState) *pState = 2;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MC_RenderFile(void *t, BSTR f) { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE MC_StubV(void *t, ...) { return E_NOTIMPL; }

static IMediaControlVtbl_DS g_FGMediaControlVtbl = {
    MC_QI, MC_AddRef, MC_Release,
    Disp_GetTypeInfoCount, Disp_GetTypeInfo, Disp_GetIDsOfNames, Disp_Invoke,
    MC_Run, MC_Pause, MC_Stop, MC_GetState,
    MC_RenderFile, (void*)MC_StubV, (void*)MC_StubV, (void*)MC_StubV, (void*)MC_StubV
};

/* ========== IMediaEvent ========== */

static HRESULT STDMETHODCALLTYPE ME_QI(void *t, REFIID r, void **p) { return FG_QI((IBaseFilter_DS*)GRAPH_FROM_ME(t),r,p); }
static ULONG STDMETHODCALLTYPE ME_AddRef(void *t) { return InterlockedIncrement(&GRAPH_FROM_ME(t)->ref_count); }
static ULONG STDMETHODCALLTYPE ME_Release(void *t) { return FG_Release((IBaseFilter_DS*)GRAPH_FROM_ME(t)); }

static HRESULT STDMETHODCALLTYPE ME_GetEventHandle(void *This, LONG_PTR *hEvent) {
    FakeGraph *g = GRAPH_FROM_ME(This);
    TRACE_MSG("FG::ME::GetEventHandle()");
    if (!hEvent) return E_POINTER;
    *hEvent = (LONG_PTR)g->complete_event;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ME_GetEvent(void *t, long *code, LONG_PTR *p1, LONG_PTR *p2, long ms) {
    TRACE_MSG("FG::ME::GetEvent(ms=%ld)", ms);
    return E_ABORT;
}
static HRESULT STDMETHODCALLTYPE ME_WaitForCompletion(void *t, long ms, long *pEvCode) {
    TRACE_MSG("FG::ME::WaitForCompletion(ms=%ld)", ms);
    if (pEvCode) *pEvCode = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ME_Stub(void *t, ...) { return S_OK; }

static IMediaEventVtbl_DS g_FGMediaEventVtbl = {
    ME_QI, ME_AddRef, ME_Release,
    (void*)Disp_GetTypeInfoCount, (void*)Disp_GetTypeInfo, (void*)Disp_GetIDsOfNames, (void*)Disp_Invoke,
    ME_GetEventHandle, ME_GetEvent, ME_WaitForCompletion,
    (void*)ME_Stub, (void*)ME_Stub, (void*)ME_Stub
};

/* ========== IMediaSeeking (delegates to source filter) ========== */

static HRESULT STDMETHODCALLTYPE FMS_QI(IMediaSeeking_DS *t, REFIID r, void **p) { return FG_QI((IBaseFilter_DS*)GRAPH_FROM_MS(t),r,p); }
static ULONG STDMETHODCALLTYPE FMS_AddRef(IMediaSeeking_DS *t) { return InterlockedIncrement(&GRAPH_FROM_MS(t)->ref_count); }
static ULONG STDMETHODCALLTYPE FMS_Release(IMediaSeeking_DS *t) { return FG_Release((IBaseFilter_DS*)GRAPH_FROM_MS(t)); }

static HRESULT get_source_seeking(FakeGraph *g, IMediaSeeking_DS **ppSeeking) {
    if (!g->source_filter) return E_FAIL;
    return g->source_filter->lpVtbl->QueryInterface(g->source_filter, &IID_IMediaSeeking, (void**)ppSeeking);
}

#define DELEGATE_MS(method, ...) \
    FakeGraph *_g = GRAPH_FROM_MS(This); \
    IMediaSeeking_DS *_sk = NULL; \
    HRESULT _qhr = get_source_seeking(_g, &_sk); \
    TRACE_MSG("FG::MS::" #method " src=%p qhr=0x%08lX srcfilt=%p", _sk, _qhr, _g->source_filter); \
    if (_qhr != S_OK || !_sk) { TRACE_MSG("  DELEGATION FAILED"); return E_FAIL; } \
    TRACE_MSG("  _sk->lpVtbl=%p", _sk->lpVtbl); \
    HRESULT _hr = _sk->lpVtbl->method(_sk, ##__VA_ARGS__); \
    _sk->lpVtbl->Release(_sk); \
    TRACE_MSG("  result=0x%08lX", _hr); \
    return _hr;

static HRESULT STDMETHODCALLTYPE FMS_GetCapabilities(IMediaSeeking_DS *This, DWORD *p) { DELEGATE_MS(GetCapabilities, p) }
static HRESULT STDMETHODCALLTYPE FMS_CheckCapabilities(IMediaSeeking_DS *This, DWORD *p) { DELEGATE_MS(CheckCapabilities, p) }
static HRESULT STDMETHODCALLTYPE FMS_IsFormatSupported(IMediaSeeking_DS *This, const GUID *f) { DELEGATE_MS(IsFormatSupported, f) }
static HRESULT STDMETHODCALLTYPE FMS_QueryPreferredFormat(IMediaSeeking_DS *This, GUID *p) { DELEGATE_MS(QueryPreferredFormat, p) }
/* The game stores IMediaSeeking but calls IMediaPosition methods through it.
   Slots [7],[8],[9] overlap: GetTimeFormat/get_Duration, IsUsingTimeFormat/put_CurrentPosition,
   SetTimeFormat/get_CurrentPosition. We handle both uses safely. */
static HRESULT STDMETHODCALLTYPE FMS_GetTimeFormat(IMediaSeeking_DS *This, GUID *p) {
    /* Game calls this as IMediaPosition::get_Duration(double*).
       Writing a 16-byte GUID to an 8-byte double* would overflow the stack.
       Return the duration as a double instead. */
    FakeGraph *g = GRAPH_FROM_MS(This);
    proxy_log("FG::MS::GetTimeFormat/get_Duration (hybrid)");
    if (!p) return E_POINTER;
    IMediaSeeking_DS *sk = NULL;
    if (g->source_filter && g->source_filter->lpVtbl->QueryInterface(
            g->source_filter, &IID_IMediaSeeking, (void**)&sk) == S_OK) {
        LONGLONG dur = 0;
        sk->lpVtbl->GetDuration(sk, &dur);
        sk->lpVtbl->Release(sk);
        double dur_sec = (double)dur / 10000000.0;
        memcpy(p, &dur_sec, sizeof(double));
        proxy_log("  -> duration=%.3f sec", dur_sec);
    } else {
        memset(p, 0, sizeof(double));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMS_IsUsingTimeFormat(IMediaSeeking_DS *This, const GUID *p) {
    /* Game also calls this as IMediaPosition::put_CurrentPosition(double).
       When p is NULL or small, treat as IsUsingTimeFormat. Always return S_OK. */
    TRACE_MSG("FG::MS::IsUsingTimeFormat/put_CurrentPosition (hybrid, p=%p)", p);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMS_SetTimeFormat(IMediaSeeking_DS *This, const GUID *p) {
    /* Game also calls this as IMediaPosition::get_CurrentPosition(double*).
       Return current position as a double to the output pointer. */
    FakeGraph *g = GRAPH_FROM_MS(This);
    proxy_log("FG::MS::SetTimeFormat/get_CurrentPosition (hybrid)");
    if (!p) return E_POINTER;
    IMediaSeeking_DS *sk = NULL;
    if (g->source_filter && g->source_filter->lpVtbl->QueryInterface(
            g->source_filter, &IID_IMediaSeeking, (void**)&sk) == S_OK) {
        LONGLONG pos = 0;
        sk->lpVtbl->GetCurrentPosition(sk, &pos);
        sk->lpVtbl->Release(sk);
        double pos_sec = (double)pos / 10000000.0;
        memcpy((void*)p, &pos_sec, sizeof(double));
        proxy_log("  -> position=%.3f sec", pos_sec);
    } else {
        memset((void*)p, 0, sizeof(double));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMS_GetDuration(IMediaSeeking_DS *This, LONGLONG *p) { DELEGATE_MS(GetDuration, p) }
static HRESULT STDMETHODCALLTYPE FMS_GetStopPosition(IMediaSeeking_DS *This, LONGLONG *p) { DELEGATE_MS(GetStopPosition, p) }
static HRESULT STDMETHODCALLTYPE FMS_GetCurrentPosition(IMediaSeeking_DS *This, LONGLONG *p) { DELEGATE_MS(GetCurrentPosition, p) }
static HRESULT STDMETHODCALLTYPE FMS_ConvertTimeFormat(IMediaSeeking_DS *This, LONGLONG *t, const GUID *tf, LONGLONG s, const GUID *sf) { DELEGATE_MS(ConvertTimeFormat, t, tf, s, sf) }
static HRESULT STDMETHODCALLTYPE FMS_SetPositions(IMediaSeeking_DS *This, LONGLONG *c, DWORD cf, LONGLONG *s, DWORD sf) { DELEGATE_MS(SetPositions, c, cf, s, sf) }
static HRESULT STDMETHODCALLTYPE FMS_GetPositions(IMediaSeeking_DS *This, LONGLONG *c, LONGLONG *s) { DELEGATE_MS(GetPositions, c, s) }
static HRESULT STDMETHODCALLTYPE FMS_GetAvailable(IMediaSeeking_DS *This, LONGLONG *a, LONGLONG *b) { DELEGATE_MS(GetAvailable, a, b) }
static HRESULT STDMETHODCALLTYPE FMS_SetRate(IMediaSeeking_DS *This, double r) { DELEGATE_MS(SetRate, r) }
static HRESULT STDMETHODCALLTYPE FMS_GetRate(IMediaSeeking_DS *This, double *p) { DELEGATE_MS(GetRate, p) }
static HRESULT STDMETHODCALLTYPE FMS_GetPreroll(IMediaSeeking_DS *This, LONGLONG *p) { DELEGATE_MS(GetPreroll, p) }

static IMediaSeekingVtbl_DS g_FGMediaSeekingVtbl = {
    FMS_QI, FMS_AddRef, FMS_Release,
    FMS_GetCapabilities, FMS_CheckCapabilities,
    FMS_IsFormatSupported, FMS_QueryPreferredFormat,
    FMS_GetTimeFormat, FMS_IsUsingTimeFormat, FMS_SetTimeFormat,
    FMS_GetDuration, FMS_GetStopPosition, FMS_GetCurrentPosition,
    FMS_ConvertTimeFormat, FMS_SetPositions, FMS_GetPositions,
    FMS_GetAvailable, FMS_SetRate, FMS_GetRate, FMS_GetPreroll
};

/* ========== IMediaPosition (delegates to source filter) ========== */

static HRESULT STDMETHODCALLTYPE FMP_QI(void *t, REFIID r, void **p) { return FG_QI((IBaseFilter_DS*)GRAPH_FROM_MP(t),r,p); }
static ULONG STDMETHODCALLTYPE FMP_AddRef(void *t) { return InterlockedIncrement(&GRAPH_FROM_MP(t)->ref_count); }
static ULONG STDMETHODCALLTYPE FMP_Release(void *t) { return FG_Release((IBaseFilter_DS*)GRAPH_FROM_MP(t)); }
static HRESULT STDMETHODCALLTYPE FMP_get_Duration(void *This, double *p) {
    TRACE_MSG("FMP_get_Duration");
    if (!p) return E_POINTER;
    FakeGraph *g = GRAPH_FROM_MP(This);
    LONGLONG d = 0;
    IMediaSeeking_DS *s = NULL;
    if (g->source_filter && g->source_filter->lpVtbl->QueryInterface(g->source_filter, &IID_IMediaSeeking, (void**)&s) == S_OK) {
        s->lpVtbl->GetDuration(s, &d);
        s->lpVtbl->Release(s);
    }
    *p = (double)d / 10000000.0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMP_get_CurrentPosition(void *This, double *p) {
    TRACE_MSG("FMP_get_CurrentPosition");
    if (!p) return E_POINTER;
    FakeGraph *g = GRAPH_FROM_MP(This);
    LONGLONG c = 0;
    IMediaSeeking_DS *s = NULL;
    if (g->source_filter && g->source_filter->lpVtbl->QueryInterface(g->source_filter, &IID_IMediaSeeking, (void**)&s) == S_OK) {
        s->lpVtbl->GetCurrentPosition(s, &c);
        s->lpVtbl->Release(s);
    }
    *p = (double)c / 10000000.0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FMP_put_CurrentPosition(void *t, double v) { proxy_log("FakeGraph::MP::put_CurrentPosition(%.3f)", v); return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_get_StopTime(void *t, double *p) { return FMP_get_Duration(t, p); }
static HRESULT STDMETHODCALLTYPE FMP_put_StopTime(void *t, double v) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_get_PrerollTime(void *t, double *p) { if(p)*p=0; return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_put_PrerollTime(void *t, double v) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_put_Rate(void *t, double r) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_get_Rate(void *t, double *p) { if(p)*p=1.0; return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_CanSeekForward(void *t, long *p) { if(p)*p=-1; return S_OK; }
static HRESULT STDMETHODCALLTYPE FMP_CanSeekBackward(void *t, long *p) { if(p)*p=-1; return S_OK; }

static IMediaPositionVtbl_FG g_FGMediaPositionVtbl = {
    FMP_QI, FMP_AddRef, FMP_Release,
    (void*)Disp_GetTypeInfoCount, (void*)Disp_GetTypeInfo, (void*)Disp_GetIDsOfNames, (void*)Disp_Invoke,
    FMP_get_Duration, FMP_put_CurrentPosition, FMP_get_CurrentPosition,
    FMP_get_StopTime, FMP_put_StopTime,
    FMP_get_PrerollTime, FMP_put_PrerollTime,
    FMP_put_Rate, FMP_get_Rate,
    FMP_CanSeekForward, FMP_CanSeekBackward
};

/* ========== IBasicAudio ========== */

static HRESULT STDMETHODCALLTYPE FBA_QI(IBasicAudio_DS *t, REFIID r, void **p) { return FG_QI((IBaseFilter_DS*)GRAPH_FROM_BA(t),r,p); }
static ULONG STDMETHODCALLTYPE FBA_AddRef(IBasicAudio_DS *t) { return InterlockedIncrement(&GRAPH_FROM_BA(t)->ref_count); }
static ULONG STDMETHODCALLTYPE FBA_Release(IBasicAudio_DS *t) { return FG_Release((IBaseFilter_DS*)GRAPH_FROM_BA(t)); }
static HRESULT STDMETHODCALLTYPE FBA_put_Volume(IBasicAudio_DS *This, long vol) {
    FakeGraph *g = GRAPH_FROM_BA(This);
    proxy_log("FakeGraph::IBasicAudio::put_Volume(%ld)", vol);
    g->volume = vol;
    /* Delegate to source filter's IBasicAudio if available */
    IBasicAudio_DS *ba = NULL;
    if (g->source_filter && g->source_filter->lpVtbl->QueryInterface(
            g->source_filter, &IID_IBasicAudio, (void**)&ba) == S_OK) {
        ba->lpVtbl->put_Volume(ba, vol);
        ba->lpVtbl->Release(ba);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FBA_get_Volume(IBasicAudio_DS *This, long *vol) {
    TRACE_MSG("FBA_get_Volume");
    if (vol) *vol = GRAPH_FROM_BA(This)->volume;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FBA_put_Balance(IBasicAudio_DS *t, long b) { return S_OK; }
static HRESULT STDMETHODCALLTYPE FBA_get_Balance(IBasicAudio_DS *t, long *b) { if(b)*b=0; return S_OK; }

static IBasicAudioVtbl_DS g_FGBasicAudioVtbl = {
    FBA_QI, FBA_AddRef, FBA_Release,
    (void*)Disp_GetTypeInfoCount, (void*)Disp_GetTypeInfo, (void*)Disp_GetIDsOfNames, (void*)Disp_Invoke,
    FBA_put_Volume, FBA_get_Volume, FBA_put_Balance, FBA_get_Balance
};

/* ========== Factory ========== */

HRESULT ds_fakegraph_create(void **ppGraph) {
    if (!ppGraph) return E_POINTER;

    FakeGraph *g = calloc(1, sizeof(FakeGraph));
    if (!g) return E_OUTOFMEMORY;

    /* Build the IGraphBuilder vtable as a raw void* array.
       IFilterGraph:  [0]QI [1]AddRef [2]Release [3]AddFilter [4]RemoveFilter
                      [5]EnumFilters [6]FindFilterByName [7]ConnectDirect
                      [8]Reconnect [9]Disconnect [10]SetDefaultSyncSource
       IGraphBuilder: [11]Connect [12]Render [13]RenderFile [14]AddSourceFilter
                      [15]SetLogFile [16]Abort [17]ShouldOperationContinue */
    g_FGGraphVtblRaw[0]  = (void*)FG_QI;
    g_FGGraphVtblRaw[1]  = (void*)FG_AddRef;
    g_FGGraphVtblRaw[2]  = (void*)FG_Release;
    g_FGGraphVtblRaw[3]  = (void*)FG_AddFilter;
    g_FGGraphVtblRaw[4]  = (void*)FG_RemoveFilter;
    g_FGGraphVtblRaw[5]  = (void*)FG_EnumFilters;
    g_FGGraphVtblRaw[6]  = (void*)FG_FindFilterByName;
    g_FGGraphVtblRaw[7]  = (void*)FG_ConnectDirect;
    g_FGGraphVtblRaw[8]  = (void*)FG_Reconnect;
    g_FGGraphVtblRaw[9]  = (void*)FG_Disconnect;
    g_FGGraphVtblRaw[10] = (void*)FG_SetDefaultSyncSource;
    g_FGGraphVtblRaw[11] = (void*)FG_Connect;
    g_FGGraphVtblRaw[12] = (void*)FG_Render;
    g_FGGraphVtblRaw[13] = (void*)FG_RenderFile;
    g_FGGraphVtblRaw[14] = (void*)FG_AddSourceFilter;
    g_FGGraphVtblRaw[15] = (void*)FG_SetLogFile;
    g_FGGraphVtblRaw[16] = (void*)FG_Abort;
    g_FGGraphVtblRaw[17] = (void*)FG_ShouldOperationContinue;

    g->lpGraphBuilderVtbl = (IBaseFilterVtbl_DS*)g_FGGraphVtblRaw;
    g->lpMediaControlVtbl = &g_FGMediaControlVtbl;
    g->lpMediaEventVtbl = &g_FGMediaEventVtbl;
    g->lpMediaSeekingVtbl = &g_FGMediaSeekingVtbl;
    g->lpMediaPositionVtbl = (void*)&g_FGMediaPositionVtbl;
    g->lpBasicAudioVtbl = &g_FGBasicAudioVtbl;
    g->ref_count = 1;
    g->complete_event = CreateEventA(NULL, TRUE, FALSE, NULL);

    *ppGraph = g;
    proxy_log("Created FakeGraph @ %p", g);
    return S_OK;
}
