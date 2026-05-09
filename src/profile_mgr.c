#include "profile_mgr.h"
#include "log.h"
#include "trace.h"
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Stub IWMStreamConfig — returned by IWMProfile methods
 * ================================================================ */

typedef struct ProxyStreamConfig {
    IWMStreamConfigVtbl *lpVtbl;
    LONG  ref_count;
    GUID  stream_type;
    WORD  stream_number;
    DWORD bitrate;
    DWORD buffer_window;
} ProxyStreamConfig;

static HRESULT STDMETHODCALLTYPE SC_QI(IWMStreamConfig *This, REFIID riid, void **ppv) {
    TRACE_MSG("SC_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWMStreamConfig)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE SC_AddRef(IWMStreamConfig *This) {
    return InterlockedIncrement(&((ProxyStreamConfig *)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE SC_Release(IWMStreamConfig *This) {
    TRACE_MSG("SC_Release");
    ProxyStreamConfig *s = (ProxyStreamConfig *)This;
    LONG ref = InterlockedDecrement(&s->ref_count);
    if (ref == 0) free(s);
    return ref;
}
static HRESULT STDMETHODCALLTYPE SC_GetStreamType(IWMStreamConfig *This, GUID *g) {
    TRACE_MSG("SC_GetStreamType");
    if (!g) return E_POINTER;
    *g = ((ProxyStreamConfig *)This)->stream_type;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_GetStreamNumber(IWMStreamConfig *This, WORD *n) {
    TRACE_MSG("SC_GetStreamNumber");
    if (!n) return E_POINTER;
    *n = ((ProxyStreamConfig *)This)->stream_number;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_SetStreamNumber(IWMStreamConfig *This, WORD n) {
    TRACE_MSG("SC_SetStreamNumber");
    ((ProxyStreamConfig *)This)->stream_number = n; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_GetStreamName(IWMStreamConfig *This, WCHAR *n, WORD *len) {
    TRACE_MSG("SC_GetStreamName");
    if (!len) return E_POINTER;
    if (!n) { *len = 1; return S_OK; }
    n[0] = L'\0'; *len = 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_SetStreamName(IWMStreamConfig *This, LPCWSTR n) { return S_OK; }
static HRESULT STDMETHODCALLTYPE SC_GetConnectionName(IWMStreamConfig *This, WCHAR *n, WORD *len) {
    TRACE_MSG("SC_GetConnectionName");
    if (!len) return E_POINTER;
    if (!n) { *len = 1; return S_OK; }
    n[0] = L'\0'; *len = 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_SetConnectionName(IWMStreamConfig *This, LPCWSTR n) { return S_OK; }
static HRESULT STDMETHODCALLTYPE SC_GetBitrate(IWMStreamConfig *This, DWORD *b) {
    TRACE_MSG("SC_GetBitrate");
    if (!b) return E_POINTER;
    *b = ((ProxyStreamConfig *)This)->bitrate; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_SetBitrate(IWMStreamConfig *This, DWORD b) {
    TRACE_MSG("SC_SetBitrate");
    ((ProxyStreamConfig *)This)->bitrate = b; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_GetBufferWindow(IWMStreamConfig *This, DWORD *w) {
    TRACE_MSG("SC_GetBufferWindow");
    if (!w) return E_POINTER;
    *w = ((ProxyStreamConfig *)This)->buffer_window; return S_OK;
}
static HRESULT STDMETHODCALLTYPE SC_SetBufferWindow(IWMStreamConfig *This, DWORD w) {
    TRACE_MSG("SC_SetBufferWindow");
    ((ProxyStreamConfig *)This)->buffer_window = w; return S_OK;
}

static IWMStreamConfigVtbl g_SCVtbl = {
    SC_QI, SC_AddRef, SC_Release,
    SC_GetStreamType, SC_GetStreamNumber, SC_SetStreamNumber,
    SC_GetStreamName, SC_SetStreamName,
    SC_GetConnectionName, SC_SetConnectionName,
    SC_GetBitrate, SC_SetBitrate,
    SC_GetBufferWindow, SC_SetBufferWindow
};

static ProxyStreamConfig *stream_config_create(const GUID *type, WORD num) {
    ProxyStreamConfig *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->lpVtbl = &g_SCVtbl;
    s->ref_count = 1;
    s->stream_type = *type;
    s->stream_number = num;
    s->bitrate = 128000;
    s->buffer_window = 3000;
    return s;
}

/* ================================================================
 * Stub IWMProfile — returned by IWMProfileManager
 * ================================================================ */

#define MAX_PROFILE_STREAMS 8

typedef struct ProxyProfile {
    IWMProfileVtbl *lpVtbl;
    LONG  ref_count;
    WMT_VERSION version;
    DWORD stream_count;
    ProxyStreamConfig *streams[MAX_PROFILE_STREAMS];
} ProxyProfile;

static HRESULT STDMETHODCALLTYPE PR_QI(IWMProfile *This, REFIID riid, void **ppv) {
    TRACE_MSG("PR_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWMProfile)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE PR_AddRef(IWMProfile *This) {
    return InterlockedIncrement(&((ProxyProfile *)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE PR_Release(IWMProfile *This) {
    TRACE_MSG("PR_Release");
    ProxyProfile *p = (ProxyProfile *)This;
    LONG ref = InterlockedDecrement(&p->ref_count);
    if (ref == 0) {
        for (DWORD i = 0; i < p->stream_count; i++)
            if (p->streams[i]) p->streams[i]->lpVtbl->Release((IWMStreamConfig *)p->streams[i]);
        free(p);
    }
    return ref;
}
static HRESULT STDMETHODCALLTYPE PR_GetVersion(IWMProfile *This, WMT_VERSION *v) {
    TRACE_MSG("PR_GetVersion");
    if (!v) return E_POINTER;
    *v = ((ProxyProfile *)This)->version; return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_GetName(IWMProfile *This, WCHAR *n, DWORD *len) {
    TRACE_MSG("PR_GetName");
    if (!len) return E_POINTER;
    if (!n) { *len = 1; return S_OK; }
    n[0] = L'\0'; *len = 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_SetName(IWMProfile *This, const WCHAR *n) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_GetDescription(IWMProfile *This, WCHAR *d, DWORD *len) {
    TRACE_MSG("PR_GetDescription");
    if (!len) return E_POINTER;
    if (!d) { *len = 1; return S_OK; }
    d[0] = L'\0'; *len = 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_SetDescription(IWMProfile *This, const WCHAR *d) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_GetStreamCount(IWMProfile *This, DWORD *c) {
    TRACE_MSG("PR_GetStreamCount");
    if (!c) return E_POINTER;
    *c = ((ProxyProfile *)This)->stream_count; return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_GetStream(IWMProfile *This, DWORD idx, IWMStreamConfig **out) {
    TRACE_MSG("PR_GetStream");
    ProxyProfile *p = (ProxyProfile *)This;
    if (!out) return E_POINTER;
    if (idx >= p->stream_count) return E_INVALIDARG;
    *out = (IWMStreamConfig *)p->streams[idx];
    p->streams[idx]->lpVtbl->AddRef((IWMStreamConfig *)p->streams[idx]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_GetStreamByNumber(IWMProfile *This, WORD num, IWMStreamConfig **out) {
    TRACE_MSG("PR_GetStreamByNumber");
    ProxyProfile *p = (ProxyProfile *)This;
    if (!out) return E_POINTER;
    for (DWORD i = 0; i < p->stream_count; i++) {
        if (p->streams[i]->stream_number == num) {
            *out = (IWMStreamConfig *)p->streams[i];
            p->streams[i]->lpVtbl->AddRef((IWMStreamConfig *)p->streams[i]);
            return S_OK;
        }
    }
    return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE PR_RemoveStream(IWMProfile *This, IWMStreamConfig *cfg) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_RemoveStreamByNumber(IWMProfile *This, WORD num) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_AddStream(IWMProfile *This, IWMStreamConfig *cfg) {
    TRACE_MSG("PR_AddStream");
    ProxyProfile *p = (ProxyProfile *)This;
    if (p->stream_count >= MAX_PROFILE_STREAMS) return E_FAIL;
    cfg->lpVtbl->AddRef(cfg);
    p->streams[p->stream_count++] = (ProxyStreamConfig *)cfg;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_ReconfigStream(IWMProfile *This, IWMStreamConfig *cfg) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_CreateNewStream(IWMProfile *This, REFGUID type, IWMStreamConfig **out) {
    TRACE_MSG("PR_CreateNewStream");
    ProxyProfile *p = (ProxyProfile *)This;
    if (!out) return E_POINTER;
    ProxyStreamConfig *sc = stream_config_create(type, (WORD)(p->stream_count + 1));
    if (!sc) return E_OUTOFMEMORY;
    *out = (IWMStreamConfig *)sc;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_GetMutualExclusionCount(IWMProfile *This, DWORD *c) {
    TRACE_MSG("PR_GetMutualExclusionCount");
    if (c) *c = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE PR_GetMutualExclusion(IWMProfile *This, DWORD i, void **p) { return E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE PR_RemoveMutualExclusion(IWMProfile *This, void *p) { return S_OK; }
static HRESULT STDMETHODCALLTYPE PR_AddMutualExclusion(IWMProfile *This, void *p) { return S_OK; }

static IWMProfileVtbl g_ProfileVtbl = {
    PR_QI, PR_AddRef, PR_Release,
    PR_GetVersion, PR_GetName, PR_SetName,
    PR_GetDescription, PR_SetDescription,
    PR_GetStreamCount, PR_GetStream, PR_GetStreamByNumber,
    PR_RemoveStream, PR_RemoveStreamByNumber,
    PR_AddStream, PR_ReconfigStream, PR_CreateNewStream,
    PR_GetMutualExclusionCount, PR_GetMutualExclusion,
    PR_RemoveMutualExclusion, PR_AddMutualExclusion
};

static ProxyProfile *profile_create(WMT_VERSION ver) {
    ProxyProfile *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->lpVtbl = &g_ProfileVtbl;
    p->ref_count = 1;
    p->version = ver;
    return p;
}

/* ================================================================
 * IWMProfileManager implementation
 * ================================================================ */

typedef struct ProxyProfileManager {
    IWMProfileManagerVtbl *lpVtbl;
    LONG ref_count;
} ProxyProfileManager;

static HRESULT STDMETHODCALLTYPE PM_QI(IWMProfileManager *This, REFIID riid, void **ppv) {
    TRACE_MSG("PM_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWMProfileManager)) {
        *ppv = This; This->lpVtbl->AddRef(This); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE PM_AddRef(IWMProfileManager *This) {
    return InterlockedIncrement(&((ProxyProfileManager *)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE PM_Release(IWMProfileManager *This) {
    TRACE_MSG("PM_Release");
    ProxyProfileManager *m = (ProxyProfileManager *)This;
    LONG ref = InterlockedDecrement(&m->ref_count);
    if (ref == 0) free(m);
    return ref;
}

static HRESULT STDMETHODCALLTYPE PM_CreateEmptyProfile(IWMProfileManager *This,
    WMT_VERSION ver, IWMProfile **ppProfile)
{
    proxy_log("ProfileManager::CreateEmptyProfile(ver=0x%08X)", ver);
    if (!ppProfile) return E_POINTER;
    ProxyProfile *p = profile_create(ver);
    if (!p) return E_OUTOFMEMORY;
    *ppProfile = (IWMProfile *)p;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE PM_LoadProfileByID(IWMProfileManager *This,
    REFGUID guidProfile, IWMProfile **ppProfile)
{
    proxy_log("ProfileManager::LoadProfileByID()");
    if (!ppProfile) return E_POINTER;
    ProxyProfile *p = profile_create(WMT_VER_9_0);
    if (!p) return E_OUTOFMEMORY;

    /* Create a default video+audio profile */
    ProxyStreamConfig *vid = stream_config_create(&WMMEDIATYPE_Video, 1);
    ProxyStreamConfig *aud = stream_config_create(&WMMEDIATYPE_Audio, 2);
    if (vid) { p->streams[p->stream_count++] = vid; }
    if (aud) { p->streams[p->stream_count++] = aud; }

    *ppProfile = (IWMProfile *)p;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE PM_LoadProfileByData(IWMProfileManager *This,
    const WCHAR *pwszProfile, IWMProfile **ppProfile)
{
    proxy_log("ProfileManager::LoadProfileByData()");
    /* The game may load profiles from XML strings (Data\Video\WMVProfile_*) */
    /* For now, return a default profile with video+audio streams */
    return PM_LoadProfileByID(This, &IID_IWMProfile /* dummy */, ppProfile);
}

static HRESULT STDMETHODCALLTYPE PM_SaveProfile(IWMProfileManager *This,
    IWMProfile *pProfile, WCHAR *pwszProfile, DWORD *pdwLength)
{
    proxy_log("ProfileManager::SaveProfile()");
    if (!pdwLength) return E_POINTER;
    /* Return a minimal XML string */
    const WCHAR *stub = L"<profile/>";
    DWORD needed = (DWORD)(wcslen(stub) + 1);
    if (!pwszProfile) { *pdwLength = needed; return S_OK; }
    if (*pdwLength < needed) { *pdwLength = needed; return ASF_E_BUFFERTOOSMALL; }
    wcscpy(pwszProfile, stub);
    *pdwLength = needed;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE PM_GetSystemProfileCount(IWMProfileManager *This, DWORD *pc) {
    TRACE_MSG("PM_GetSystemProfileCount");
    if (pc) *pc = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE PM_LoadSystemProfile(IWMProfileManager *This,
    DWORD idx, IWMProfile **ppProfile)
{
    return E_INVALIDARG;
}

static IWMProfileManagerVtbl g_PMVtbl = {
    PM_QI, PM_AddRef, PM_Release,
    PM_CreateEmptyProfile,
    PM_LoadProfileByID,
    PM_LoadProfileByData,
    PM_SaveProfile,
    PM_GetSystemProfileCount,
    PM_LoadSystemProfile
};

HRESULT proxy_profile_manager_create(IWMProfileManager **ppMgr) {
    if (!ppMgr) return E_POINTER;
    ProxyProfileManager *m = calloc(1, sizeof(*m));
    if (!m) return E_OUTOFMEMORY;
    m->lpVtbl = &g_PMVtbl;
    m->ref_count = 1;
    *ppMgr = (IWMProfileManager *)m;
    proxy_log("Created ProxyProfileManager @ %p", m);
    return S_OK;
}
