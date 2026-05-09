#include "profile_mgr.h"
#include "log.h"
#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ================================================================
 * Stub IWMStreamConfig — returned by IWMProfile methods
 * ================================================================ */

typedef struct ProxyStreamConfig {
    IWMStreamConfigVtbl *lpVtbl;
    LONG  ref_count;
    GUID  stream_type;       /* MEDIATYPE_Video / MEDIATYPE_Audio */
    GUID  sub_type;          /* MEDIASUBTYPE_WMV3 / MEDIASUBTYPE_WMAUDIO2 */
    WORD  stream_number;
    DWORD bitrate;
    DWORD buffer_window;

    /* Video fields (set by .prx parser when stream_type == Video) */
    int      width;
    int      height;
    LONGLONG avg_time_per_frame;   /* 100-ns units */
    DWORD    video_quality;        /* 0-100 from <videomediaprops> */
    char     compression[8];       /* e.g. "WMV3" */

    /* Audio fields (set by .prx parser when stream_type == Audio) */
    WORD  format_tag;              /* 0x161 = WMA2 */
    WORD  channels;
    DWORD samples_per_sec;
    DWORD avg_bytes_per_sec;
    WORD  block_align;
    WORD  bits_per_sample;
    BYTE  codec_data[64];
    int   codec_data_size;
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

/* ================================================================
 * .prx (WMV profile) XML parser
 *
 * Game ships profiles inside Data\pak\*.pak. Format is UTF-16 LE
 * with BOM. Standard Microsoft WMV-profile XML schema. Extracted
 * samples + schema in mod/STATUS.md.
 *
 * We only parse what the writer (#2) needs to drive the encoder.
 * ================================================================ */

/* Copy a wide-char attribute value into an ASCII buffer.
   Looks up `attr="..."` within `haystack` and ASCII-fies the value. */
static int xml_attr_str(const WCHAR *haystack, const WCHAR *attr,
                        char *dst, size_t dst_size)
{
    if (!haystack) return 0;
    size_t alen = wcslen(attr);
    /* Build the full needle: attr="    */
    WCHAR needle[64];
    if (alen + 3 >= sizeof(needle) / sizeof(needle[0])) return 0;
    wcscpy(needle, attr);
    wcscat(needle, L"=\"");
    const WCHAR *p = wcsstr(haystack, needle);
    if (!p) return 0;
    p += wcslen(needle);
    const WCHAR *end = wcschr(p, L'"');
    if (!end) return 0;
    size_t n = (size_t)(end - p);
    if (n >= dst_size) n = dst_size - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (char)p[i];
    dst[n] = 0;
    return (int)n;
}

static long long xml_attr_int(const WCHAR *haystack, const WCHAR *attr) {
    char buf[32];
    if (!xml_attr_str(haystack, attr, buf, sizeof(buf))) return 0;
    return _atoi64(buf);
}

/* Locate the next `<tag` in haystack and return the substring
   spanning from that '<' to the matching `</tag>` (or `/>`).
   *next_out is set to one past the end of that block, suitable for
   continuing the scan. Returns NULL if not found. */
static const WCHAR *xml_find_block(const WCHAR *haystack, const WCHAR *tag,
                                   const WCHAR **next_out)
{
    if (!haystack) return NULL;
    WCHAR open[32], close[32];
    if (wcslen(tag) >= 28) return NULL;
    wcscpy(open,  L"<");
    wcscat(open,  tag);
    wcscpy(close, L"</");
    wcscat(close, tag);
    wcscat(close, L">");
    const WCHAR *start = wcsstr(haystack, open);
    if (!start) return NULL;
    /* Skip ahead past the opening `<tag` to find the closing `</tag>` or
       a self-closing `/>`. We don't need to *return* the closing point
       precisely — we return the block from start onward and let the
       caller carry on scanning. Move next_out past the start so a
       repeated search proceeds. */
    if (next_out) *next_out = start + wcslen(open);
    return start;
}

/* Map an XML majortype GUID (in `{HHHHHHHH-HHHH-…}` form) to one of our
   well-known media-type GUIDs. Sets *out and returns 1 on hit, 0 else. */
static int classify_majortype(const char *guid_str, GUID *out) {
    /* The .prx files use the "fourcc-as-Data1" pattern: video majortype
       Data1 = 'vids' = 0x73646976; audio = 'auds' = 0x73647561. */
    if (!guid_str) return 0;
    if (strncmp(guid_str + 1, "73646976", 8) == 0) {
        *out = WMMEDIATYPE_Video; return 1;
    }
    if (strncmp(guid_str + 1, "73647561", 8) == 0) {
        *out = WMMEDIATYPE_Audio; return 1;
    }
    return 0;
}

static int classify_subtype(const char *guid_str, GUID *out) {
    if (!guid_str) return 0;
    if (strncmp(guid_str + 1, "33564D57", 8) == 0) { /* WMV3 */
        *out = MEDIASUBTYPE_WMV3; return 1;
    }
    if (strncmp(guid_str + 1, "00000161", 8) == 0) { /* WMA2 */
        *out = MEDIASUBTYPE_WMAUDIO2; return 1;
    }
    return 0;
}

/* Parse one <streamconfig>...</streamconfig> block. Caller passes the
   substring starting at the '<streamconfig'. Returns the new
   ProxyStreamConfig (refcount=1) on success, NULL on failure. */
static ProxyStreamConfig *parse_streamconfig(const WCHAR *block) {
    ProxyStreamConfig *sc = calloc(1, sizeof(*sc));
    if (!sc) return NULL;
    sc->lpVtbl    = &g_SCVtbl;
    sc->ref_count = 1;

    char guid_buf[64];

    /* <streamconfig> attributes */
    if (xml_attr_str(block, L"majortype", guid_buf, sizeof(guid_buf)))
        classify_majortype(guid_buf, &sc->stream_type);
    sc->stream_number = (WORD)xml_attr_int(block, L"streamnumber");
    sc->bitrate       = (DWORD)xml_attr_int(block, L"bitrate");
    sc->buffer_window = (DWORD)xml_attr_int(block, L"bufferwindow");

    /* <wmmediatype subtype="..."> */
    const WCHAR *next = NULL;
    const WCHAR *mt = xml_find_block(block, L"wmmediatype", &next);
    if (mt && xml_attr_str(mt, L"subtype", guid_buf, sizeof(guid_buf)))
        classify_subtype(guid_buf, &sc->sub_type);

    if (IsEqualGUID(&sc->stream_type, &WMMEDIATYPE_Audio)) {
        /* <waveformatex …> */
        const WCHAR *wfx = xml_find_block(block, L"waveformatex", &next);
        if (wfx) {
            sc->format_tag        = (WORD) xml_attr_int(wfx, L"wFormatTag");
            sc->channels          = (WORD) xml_attr_int(wfx, L"nChannels");
            sc->samples_per_sec   = (DWORD)xml_attr_int(wfx, L"nSamplesPerSec");
            sc->avg_bytes_per_sec = (DWORD)xml_attr_int(wfx, L"nAvgBytesPerSec");
            sc->block_align       = (WORD) xml_attr_int(wfx, L"nBlockAlign");
            sc->bits_per_sample   = (WORD) xml_attr_int(wfx, L"wBitsPerSample");
            /* codec_data is hex-encoded; not strictly needed for FFmpeg's
               wmav2 encoder, but we capture length so the writer can
               forward it if the muxer wants it. Skip the hex parse for
               now — we'll add it iff #2 needs it. */
        }
    } else if (IsEqualGUID(&sc->stream_type, &WMMEDIATYPE_Video)) {
        /* <videomediaprops quality=…> */
        const WCHAR *vmp = xml_find_block(block, L"videomediaprops", &next);
        if (vmp) sc->video_quality = (DWORD)xml_attr_int(vmp, L"quality");

        /* <videoinfoheader avgtimeperframe=…> */
        const WCHAR *vih = xml_find_block(block, L"videoinfoheader", &next);
        if (vih) sc->avg_time_per_frame = xml_attr_int(vih, L"avgtimeperframe");

        /* <bitmapinfoheader biwidth=… biheight=… bicompression=…> */
        const WCHAR *bih = xml_find_block(block, L"bitmapinfoheader", &next);
        if (bih) {
            sc->width  = (int)xml_attr_int(bih, L"biwidth");
            sc->height = (int)xml_attr_int(bih, L"biheight");
            xml_attr_str(bih, L"bicompression",
                         sc->compression, sizeof(sc->compression));
        }
    }
    return sc;
}

/* Parse the complete .prx XML and populate ProxyProfile->streams[].
   `xml_data` is whatever the game passed to LoadProfileByData (may
   include a UTF-16 BOM). Returns count of streams populated. */
static int parse_prx_profile(const WCHAR *xml_data, ProxyProfile *p) {
    if (!xml_data || !p) return 0;
    /* Skip optional UTF-16 BOM. */
    if (*xml_data == 0xFEFF) xml_data++;

    int count = 0;
    const WCHAR *cursor = xml_data;
    while (count < MAX_PROFILE_STREAMS) {
        const WCHAR *next = NULL;
        const WCHAR *block = xml_find_block(cursor, L"streamconfig", &next);
        if (!block) break;
        ProxyStreamConfig *sc = parse_streamconfig(block);
        if (sc) {
            p->streams[p->stream_count++] = sc;
            count++;
            const char *type =
                IsEqualGUID(&sc->stream_type, &WMMEDIATYPE_Video) ? "video" :
                IsEqualGUID(&sc->stream_type, &WMMEDIATYPE_Audio) ? "audio" :
                                                                     "?";
            if (IsEqualGUID(&sc->stream_type, &WMMEDIATYPE_Video))
                proxy_log("  parsed %s stream #%d: %dx%d %s @ %lld00ns/frame, %lu bps, q=%lu",
                          type, sc->stream_number, sc->width, sc->height,
                          sc->compression, (long long)sc->avg_time_per_frame,
                          (unsigned long)sc->bitrate, (unsigned long)sc->video_quality);
            else
                proxy_log("  parsed %s stream #%d: %luHz x%d %dbit, fmt=0x%X, %lu bps",
                          type, sc->stream_number,
                          (unsigned long)sc->samples_per_sec, sc->channels,
                          sc->bits_per_sample, sc->format_tag,
                          (unsigned long)sc->bitrate);
        }
        cursor = next;
    }
    return count;
}

static HRESULT STDMETHODCALLTYPE PM_LoadProfileByData(IWMProfileManager *This,
    const WCHAR *pwszProfile, IWMProfile **ppProfile)
{
    if (!ppProfile) return E_POINTER;

    ProxyProfile *p = profile_create(WMT_VER_9_0);
    if (!p) return E_OUTOFMEMORY;

    int n = parse_prx_profile(pwszProfile, p);
    proxy_log("ProfileManager::LoadProfileByData() -> %d streams parsed", n);
    if (n == 0) {
        /* Parser found nothing — fall back to a default video+audio
           profile so legacy callers get *something*. */
        ProxyStreamConfig *vid = stream_config_create(&WMMEDIATYPE_Video, 1);
        ProxyStreamConfig *aud = stream_config_create(&WMMEDIATYPE_Audio, 2);
        if (vid) p->streams[p->stream_count++] = vid;
        if (aud) p->streams[p->stream_count++] = aud;
        proxy_log("  (no streams parsed; falling back to default video+audio)");
    }

    *ppProfile = (IWMProfile *)p;
    return S_OK;
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
