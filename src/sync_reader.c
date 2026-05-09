#include "sync_reader.h"
#include "nss_buffer.h"
#include "media_props.h"
#include "log.h"
#include "trace.h"

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

#include <stdlib.h>
#include <string.h>

#define MAX_STREAMS 16
#define AVIO_BUF_SIZE 32768

typedef struct StreamInfo {
    int                   av_stream_idx;
    WMT_STREAM_SELECTION  selection;
    BOOL                  is_video;
    AVCodecContext       *codec_ctx;
    struct SwsContext    *sws_ctx;
    int                   out_width;
    int                   out_height;
    int                   out_bpp;
    struct SwrContext    *swr_ctx;
    int                   out_sample_rate;
    int                   out_channels;
    int                   out_bits_per_sample;
} StreamInfo;

/* Forward declare — the reader also exposes IWMHeaderInfo3 via QI */
typedef struct ProxySyncReader ProxySyncReader;
typedef struct ProxyHeaderInfo ProxyHeaderInfo;

/* ========== IWMHeaderInfo3 stub (only Duration is used) ========== */

typedef struct IWMHeaderInfo3Vtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ProxyHeaderInfo*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ProxyHeaderInfo*);
    ULONG   (STDMETHODCALLTYPE *Release)(ProxyHeaderInfo*);
    /* IWMHeaderInfo */
    HRESULT (STDMETHODCALLTYPE *GetAttributeCount)(ProxyHeaderInfo*, WORD, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetAttributeByIndex)(ProxyHeaderInfo*, WORD, WORD*, WCHAR*, WORD*, WMT_ATTR_DATATYPE*, BYTE*, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetAttributeByName)(ProxyHeaderInfo*, WORD*, LPCWSTR, WMT_ATTR_DATATYPE*, BYTE*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetAttribute)(ProxyHeaderInfo*, WORD, LPCWSTR, WMT_ATTR_DATATYPE, const BYTE*, WORD);
    /* IWMHeaderInfo2 */
    HRESULT (STDMETHODCALLTYPE *GetCodecInfoCount)(ProxyHeaderInfo*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetCodecInfo)(ProxyHeaderInfo*, DWORD, WORD*, WCHAR*, WORD*, WCHAR*, void*, WORD*, DWORD*);
    /* IWMHeaderInfo3 */
    HRESULT (STDMETHODCALLTYPE *GetAttributeCountEx)(ProxyHeaderInfo*, WORD, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetAttributeIndices)(ProxyHeaderInfo*, WORD, LPCWSTR, WORD*, WORD*, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetAttributeByIndexEx)(ProxyHeaderInfo*, WORD, WORD, WCHAR*, WORD*, WMT_ATTR_DATATYPE*, WORD*, BYTE*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *ModifyAttribute)(ProxyHeaderInfo*, WORD, WORD, WMT_ATTR_DATATYPE, WORD, const BYTE*, DWORD);
    HRESULT (STDMETHODCALLTYPE *AddAttribute)(ProxyHeaderInfo*, WORD, LPCWSTR, WORD*, WMT_ATTR_DATATYPE, WORD, const BYTE*, DWORD);
    HRESULT (STDMETHODCALLTYPE *DeleteAttribute)(ProxyHeaderInfo*, WORD, WORD);
    HRESULT (STDMETHODCALLTYPE *AddCodecInfo)(ProxyHeaderInfo*, LPCWSTR, LPCWSTR, void*, DWORD, DWORD);
} IWMHeaderInfo3Vtbl;

struct ProxyHeaderInfo {
    IWMHeaderInfo3Vtbl *lpVtbl;
    ProxySyncReader    *owner;
};

/* ========== ProxySyncReader ========== */

struct ProxySyncReader {
    IWMSyncReaderVtbl *lpVtbl;
    LONG               ref_count;

    AVFormatContext   *fmt_ctx;
    DWORD              output_count;
    StreamInfo         streams[MAX_STREAMS];

    AVPacket          *pkt;
    AVFrame           *frame;
    BOOL               eof;

    QWORD              range_start;
    LONGLONG           range_duration;

    /* IStream-backed AVIO */
    IStream           *source_stream;
    AVIOContext       *avio_ctx;
    unsigned char     *avio_buf;

    /* Duration in 100ns units */
    QWORD              duration_100ns;

    /* Embedded header info for QI */
    ProxyHeaderInfo    header_info;
};

/* ========== IStream → FFmpeg AVIO bridge ========== */

static int avio_read_istream(void *opaque, uint8_t *buf, int buf_size) {
    IStream *stm = (IStream *)opaque;
    ULONG bytes_read = 0;
    HRESULT hr = stm->lpVtbl->Read(stm, buf, (ULONG)buf_size, &bytes_read);
    if (FAILED(hr) || bytes_read == 0) return AVERROR_EOF;
    return (int)bytes_read;
}

static int64_t avio_seek_istream(void *opaque, int64_t offset, int whence) {
    IStream *stm = (IStream *)opaque;
    LARGE_INTEGER li;
    ULARGE_INTEGER new_pos;
    DWORD origin;

    if (whence == AVSEEK_SIZE) {
        STATSTG stat;
        if (SUCCEEDED(stm->lpVtbl->Stat(stm, &stat, STATFLAG_NONAME)))
            return (int64_t)stat.cbSize.QuadPart;
        return -1;
    }

    switch (whence) {
    case SEEK_SET: origin = STREAM_SEEK_SET; break;
    case SEEK_CUR: origin = STREAM_SEEK_CUR; break;
    case SEEK_END: origin = STREAM_SEEK_END; break;
    default: return -1;
    }
    li.QuadPart = offset;
    if (FAILED(stm->lpVtbl->Seek(stm, li, origin, &new_pos))) return -1;
    return (int64_t)new_pos.QuadPart;
}

/* ========== helpers ========== */

static void stream_cleanup(StreamInfo *s) {
    if (s->codec_ctx) avcodec_free_context(&s->codec_ctx);
    if (s->sws_ctx)   { sws_freeContext(s->sws_ctx); s->sws_ctx = NULL; }
    if (s->swr_ctx)   swr_free(&s->swr_ctx);
}

static void reader_close_internal(ProxySyncReader *r) {
    for (DWORD i = 0; i < r->output_count; i++)
        stream_cleanup(&r->streams[i]);
    r->output_count = 0;
    if (r->pkt)     av_packet_free(&r->pkt);
    if (r->frame)   av_frame_free(&r->frame);
    if (r->fmt_ctx) avformat_close_input(&r->fmt_ctx);
    if (r->avio_ctx) {
        av_freep(&r->avio_ctx->buffer);
        avio_context_free(&r->avio_ctx);
        r->avio_buf = NULL;
    }
    if (r->source_stream) {
        r->source_stream->lpVtbl->Release(r->source_stream);
        r->source_stream = NULL;
    }
    r->eof = FALSE;
    r->duration_100ns = 0;
}

static BOOL open_codec(StreamInfo *si, AVStream *avs) {
    const AVCodec *dec = avcodec_find_decoder(avs->codecpar->codec_id);
    if (!dec) {
        proxy_log("  no decoder for codec_id %d", avs->codecpar->codec_id);
        return FALSE;
    }
    si->codec_ctx = avcodec_alloc_context3(dec);
    if (!si->codec_ctx) return FALSE;
    if (avcodec_parameters_to_context(si->codec_ctx, avs->codecpar) < 0) return FALSE;
    if (avcodec_open2(si->codec_ctx, dec, NULL) < 0) return FALSE;
    return TRUE;
}

static void setup_video_output(StreamInfo *si) {
    si->out_width  = si->codec_ctx->width;
    si->out_height = si->codec_ctx->height;
    si->out_bpp    = 24;
    si->sws_ctx = sws_getContext(
        si->codec_ctx->width, si->codec_ctx->height, si->codec_ctx->pix_fmt,
        si->out_width, si->out_height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, NULL, NULL, NULL);
}

static void setup_audio_output(StreamInfo *si) {
    si->out_sample_rate     = si->codec_ctx->sample_rate;
    si->out_channels        = si->codec_ctx->ch_layout.nb_channels;
    if (si->out_channels == 0) si->out_channels = 2;
    si->out_bits_per_sample = 16;

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, si->out_channels);
    swr_alloc_set_opts2(&si->swr_ctx,
        &out_layout, AV_SAMPLE_FMT_S16, si->out_sample_rate,
        &si->codec_ctx->ch_layout, si->codec_ctx->sample_fmt, si->codec_ctx->sample_rate,
        0, NULL);
    if (si->swr_ctx) swr_init(si->swr_ctx);
}

static void fill_video_media_type(WM_MEDIA_TYPE *mt, VIDEOINFOHEADER *vih, StreamInfo *si) {
    memset(mt, 0, sizeof(*mt));
    mt->majortype = WMMEDIATYPE_Video;
    mt->subtype = WMMEDIASUBTYPE_RGB24;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = si->out_width * si->out_height * (si->out_bpp / 8);
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE *)vih;

    memset(vih, 0, sizeof(*vih));
    if (si->codec_ctx) {
        AVRational fr = si->codec_ctx->framerate;
        if (fr.num > 0 && fr.den > 0)
            vih->AvgTimePerFrame = (LONGLONG)(10000000.0 * fr.den / fr.num);
        else
            vih->AvgTimePerFrame = 333333;
    }
    vih->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth       = si->out_width;
    vih->bmiHeader.biHeight      = si->out_height;
    vih->bmiHeader.biPlanes      = 1;
    vih->bmiHeader.biBitCount    = (WORD)si->out_bpp;
    vih->bmiHeader.biCompression = BI_RGB;
    vih->bmiHeader.biSizeImage   = mt->lSampleSize;
}

static void fill_audio_media_type(WM_MEDIA_TYPE *mt, WAVEFORMATEX *wfx, StreamInfo *si) {
    memset(mt, 0, sizeof(*mt));
    mt->majortype = WMMEDIATYPE_Audio;
    mt->subtype = WMMEDIASUBTYPE_PCM;
    mt->bFixedSizeSamples = TRUE;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(WAVEFORMATEX);
    mt->pbFormat = (BYTE *)wfx;

    memset(wfx, 0, sizeof(*wfx));
    wfx->wFormatTag      = WAVE_FORMAT_PCM;
    wfx->nChannels       = (WORD)si->out_channels;
    wfx->nSamplesPerSec  = si->out_sample_rate;
    wfx->wBitsPerSample  = (WORD)si->out_bits_per_sample;
    wfx->nBlockAlign     = wfx->nChannels * wfx->wBitsPerSample / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    mt->lSampleSize = wfx->nBlockAlign;
}

/* Open from an already-configured fmt_ctx (shared by Open and OpenStream) */
static HRESULT reader_open_internal(ProxySyncReader *r) {
    if (avformat_find_stream_info(r->fmt_ctx, NULL) < 0) {
        proxy_log("  avformat_find_stream_info failed");
        avformat_close_input(&r->fmt_ctx);
        return E_FAIL;
    }

    /* Compute duration in 100ns units */
    if (r->fmt_ctx->duration > 0)
        r->duration_100ns = (QWORD)(r->fmt_ctx->duration * 10); /* us -> 100ns */
    else
        r->duration_100ns = 0;

    r->output_count = 0;
    for (unsigned i = 0; i < r->fmt_ctx->nb_streams && r->output_count < MAX_STREAMS; i++) {
        AVStream *avs = r->fmt_ctx->streams[i];
        if (avs->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            avs->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        StreamInfo *si = &r->streams[r->output_count];
        memset(si, 0, sizeof(*si));
        si->av_stream_idx = (int)i;
        si->selection = WMT_ON;
        si->is_video = (avs->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        if (!open_codec(si, avs)) {
            proxy_log("  failed to open codec for stream %u", i);
            stream_cleanup(si);
            continue;
        }

        if (si->is_video) {
            setup_video_output(si);
            proxy_log("  video stream %u: %dx%d", i, si->out_width, si->out_height);
        } else {
            setup_audio_output(si);
            proxy_log("  audio stream %u: %dHz %dch", i, si->out_sample_rate, si->out_channels);
        }
        r->output_count++;
    }

    r->pkt   = av_packet_alloc();
    r->frame = av_frame_alloc();
    r->eof   = FALSE;
    r->range_start    = 0;
    r->range_duration = -1;

    proxy_log("  opened: %lu outputs, duration=%llu00ns", r->output_count,
              (unsigned long long)r->duration_100ns);
    return S_OK;
}

/* ========== IWMHeaderInfo3 implementation ========== */

static HRESULT STDMETHODCALLTYPE HI_QI(ProxyHeaderInfo *This, REFIID riid, void **ppv) {
    TRACE_MSG("HI_QI");
    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IWMHeaderInfo) ||
        IsEqualGUID(riid, &IID_IWMHeaderInfo3)) {
        *ppv = This;
        InterlockedIncrement(&This->owner->ref_count);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE HI_AddRef(ProxyHeaderInfo *This) {
    return InterlockedIncrement(&This->owner->ref_count);
}

static ULONG STDMETHODCALLTYPE HI_Release(ProxyHeaderInfo *This) {
    return ((IWMSyncReader *)This->owner)->lpVtbl->Release((IWMSyncReader *)This->owner);
}

static HRESULT STDMETHODCALLTYPE HI_GetAttributeCount(ProxyHeaderInfo *This, WORD stream, WORD *count) {
    TRACE_MSG("HI_GetAttributeCount");
    if (count) *count = 1; /* just Duration */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE HI_GetAttributeByIndex(ProxyHeaderInfo *This,
    WORD idx, WORD *pStream, WCHAR *pName, WORD *pcchName,
    WMT_ATTR_DATATYPE *pType, BYTE *pValue, WORD *pcbLen)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE HI_GetAttributeByName(ProxyHeaderInfo *This,
    WORD *pStream, LPCWSTR pszName, WMT_ATTR_DATATYPE *pType, BYTE *pValue, WORD *pcbLen)
{
    TRACE_MSG("HI_GetAttributeByName");
    if (!pszName || !pcbLen) return E_POINTER;

    if (wcscmp(pszName, L"Duration") == 0) {
        proxy_log("HeaderInfo::GetAttributeByName(\"Duration\") -> %llu",
                  (unsigned long long)This->owner->duration_100ns);
        if (pType) *pType = WMT_TYPE_QWORD;
        if (!pValue) { *pcbLen = sizeof(QWORD); return S_OK; }
        if (*pcbLen < sizeof(QWORD)) { *pcbLen = sizeof(QWORD); return ASF_E_BUFFERTOOSMALL; }
        *(QWORD *)pValue = This->owner->duration_100ns;
        *pcbLen = sizeof(QWORD);
        return S_OK;
    }

    proxy_log("HeaderInfo::GetAttributeByName(\"%ls\") -> E_NOTIMPL", pszName);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE HI_SetAttribute(ProxyHeaderInfo *This,
    WORD stream, LPCWSTR name, WMT_ATTR_DATATYPE type, const BYTE *val, WORD len)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_GetCodecInfoCount(ProxyHeaderInfo *This, DWORD *c)
{ if (c) *c = 0; return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_GetCodecInfo(ProxyHeaderInfo *This,
    DWORD idx, WORD *a, WCHAR *b, WORD *c, WCHAR *d, void *e, WORD *f, DWORD *g)
{ return E_INVALIDARG; }

static HRESULT STDMETHODCALLTYPE HI_GetAttributeCountEx(ProxyHeaderInfo *This, WORD s, WORD *c)
{ if (c) *c = 1; return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_GetAttributeIndices(ProxyHeaderInfo *This,
    WORD s, LPCWSTR name, WORD *lang, WORD *indices, WORD *count)
{ return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE HI_GetAttributeByIndexEx(ProxyHeaderInfo *This,
    WORD s, WORD i, WCHAR *name, WORD *namelen, WMT_ATTR_DATATYPE *type,
    WORD *lang, BYTE *val, DWORD *vallen)
{ return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE HI_ModifyAttribute(ProxyHeaderInfo *This,
    WORD s, WORD i, WMT_ATTR_DATATYPE t, WORD l, const BYTE *v, DWORD vl)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_AddAttribute(ProxyHeaderInfo *This,
    WORD s, LPCWSTR n, WORD *i, WMT_ATTR_DATATYPE t, WORD l, const BYTE *v, DWORD vl)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_DeleteAttribute(ProxyHeaderInfo *This, WORD s, WORD i)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE HI_AddCodecInfo(ProxyHeaderInfo *This,
    LPCWSTR a, LPCWSTR b, void *c, DWORD d, DWORD e)
{ return S_OK; }

static IWMHeaderInfo3Vtbl g_HeaderInfoVtbl = {
    HI_QI, HI_AddRef, HI_Release,
    HI_GetAttributeCount, HI_GetAttributeByIndex,
    HI_GetAttributeByName, HI_SetAttribute,
    HI_GetCodecInfoCount, HI_GetCodecInfo,
    HI_GetAttributeCountEx, HI_GetAttributeIndices,
    HI_GetAttributeByIndexEx, HI_ModifyAttribute,
    HI_AddAttribute, HI_DeleteAttribute, HI_AddCodecInfo
};

/* ========== IWMSyncReader: IUnknown ========== */

static HRESULT STDMETHODCALLTYPE SR_QI(IWMSyncReader *This, REFIID riid, void **ppv) {
    TRACE_MSG("SR_QI");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IWMSyncReader)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IWMHeaderInfo) || IsEqualGUID(riid, &IID_IWMHeaderInfo3)) {
        proxy_log("SyncReader::QI(IWMHeaderInfo3) -> OK");
        *ppv = &r->header_info;
        InterlockedIncrement(&r->ref_count);
        return S_OK;
    }
    proxy_log("SyncReader::QI({%08lX-...}) -> E_NOINTERFACE",
              riid->Data1);
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE SR_AddRef(IWMSyncReader *This) {
    return InterlockedIncrement(&((ProxySyncReader *)This)->ref_count);
}

static ULONG STDMETHODCALLTYPE SR_Release(IWMSyncReader *This) {
    TRACE_MSG("SR_Release");
    ProxySyncReader *r = (ProxySyncReader *)This;
    LONG ref = InterlockedDecrement(&r->ref_count);
    if (ref == 0) {
        reader_close_internal(r);
        free(r);
    }
    return ref;
}

/* ========== IWMSyncReader methods ========== */

static HRESULT STDMETHODCALLTYPE SR_Open(IWMSyncReader *This, const WCHAR *path) {
    TRACE_MSG("SR_Open");
    ProxySyncReader *r = (ProxySyncReader *)This;
    reader_close_internal(r);

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    char *utf8_path = malloc(utf8_len);
    if (!utf8_path) return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8_path, utf8_len, NULL, NULL);

    proxy_log("SyncReader::Open(\"%s\")", utf8_path);

    int ret = avformat_open_input(&r->fmt_ctx, utf8_path, NULL, NULL);
    free(utf8_path);
    if (ret < 0) {
        proxy_log("  avformat_open_input failed: %d", ret);
        return E_FAIL;
    }
    return reader_open_internal(r);
}

static HRESULT STDMETHODCALLTYPE SR_Close(IWMSyncReader *This) {
    proxy_log("SyncReader::Close()");
    reader_close_internal((ProxySyncReader *)This);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_SetRange(IWMSyncReader *This, QWORD start, LONGLONG duration) {
    TRACE_MSG("SR_SetRange");
    ProxySyncReader *r = (ProxySyncReader *)This;
    proxy_log("SyncReader::SetRange(%llu, %lld)", (unsigned long long)start, (long long)duration);
    r->range_start = start;
    r->range_duration = duration;
    if (r->fmt_ctx) {
        int64_t ts = (int64_t)(start / 10);
        av_seek_frame(r->fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
        for (DWORD i = 0; i < r->output_count; i++)
            if (r->streams[i].codec_ctx)
                avcodec_flush_buffers(r->streams[i].codec_ctx);
        r->eof = FALSE;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_SetRangeByFrame(IWMSyncReader *This, WORD stream, QWORD frame, LONGLONG count) {
    proxy_log("SyncReader::SetRangeByFrame(stream=%u, frame=%llu)", stream, (unsigned long long)frame);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetNextSample(IWMSyncReader *This, WORD wStreamNum,
    INSSBuffer **ppSample, QWORD *pcnsSampleTime, QWORD *pcnsDuration,
    DWORD *pdwFlags, DWORD *pdwOutputNum, WORD *pwStreamNum)
{
    TRACE_MSG("SR_GetNextSample");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (!r->fmt_ctx) return NS_E_INVALID_REQUEST;
    if (r->eof) return S_FALSE;

    while (1) {
        int ret = av_read_frame(r->fmt_ctx, r->pkt);
        if (ret < 0) {
            r->eof = TRUE;
            return S_FALSE;
        }

        DWORD out_idx = (DWORD)-1;
        for (DWORD i = 0; i < r->output_count; i++) {
            if (r->streams[i].av_stream_idx == r->pkt->stream_index) {
                out_idx = i;
                break;
            }
        }

        if (out_idx == (DWORD)-1 || r->streams[out_idx].selection == WMT_OFF) {
            av_packet_unref(r->pkt);
            continue;
        }

        StreamInfo *si = &r->streams[out_idx];
        ret = avcodec_send_packet(si->codec_ctx, r->pkt);
        av_packet_unref(r->pkt);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(si->codec_ctx, r->frame);
        if (ret < 0) continue;

        AVStream *avs = r->fmt_ctx->streams[si->av_stream_idx];
        QWORD sample_time = 0;
        if (r->frame->pts != AV_NOPTS_VALUE)
            sample_time = (QWORD)(r->frame->pts * av_q2d(avs->time_base) * 10000000.0);

        ProxyNSSBuffer *buf = NULL;

        if (si->is_video) {
            DWORD frame_size = si->out_width * si->out_height * (si->out_bpp / 8);
            buf = nss_buffer_create(frame_size);
            if (!buf) { av_frame_unref(r->frame); return E_OUTOFMEMORY; }

            int dst_stride[1] = { si->out_width * 3 };
            uint8_t *dst_data[1] = { buf->data + (si->out_height - 1) * dst_stride[0] };
            int neg_stride[1] = { -dst_stride[0] };
            sws_scale(si->sws_ctx, (const uint8_t *const *)r->frame->data,
                      r->frame->linesize, 0, si->codec_ctx->height,
                      dst_data, neg_stride);
            buf->length = frame_size;
        } else {
            int out_samples = swr_get_out_samples(si->swr_ctx, r->frame->nb_samples);
            DWORD buf_size = out_samples * si->out_channels * (si->out_bits_per_sample / 8);
            buf = nss_buffer_create(buf_size);
            if (!buf) { av_frame_unref(r->frame); return E_OUTOFMEMORY; }

            uint8_t *out_buf[1] = { buf->data };
            int converted = swr_convert(si->swr_ctx, out_buf, out_samples,
                (const uint8_t **)r->frame->extended_data, r->frame->nb_samples);
            buf->length = (converted > 0) ?
                converted * si->out_channels * (si->out_bits_per_sample / 8) : 0;
        }
        av_frame_unref(r->frame);

        if (ppSample)       *ppSample = (INSSBuffer *)buf;
        else                buf->lpVtbl->Release((INSSBuffer *)buf);
        if (pcnsSampleTime) *pcnsSampleTime = sample_time;
        if (pcnsDuration)   *pcnsDuration = 0;
        if (pdwFlags)       *pdwFlags = 0;
        if (pdwOutputNum)   *pdwOutputNum = out_idx;
        if (pwStreamNum)    *pwStreamNum = (WORD)(out_idx + 1);
        return S_OK;
    }
}

static HRESULT STDMETHODCALLTYPE SR_SetStreamsSelected(IWMSyncReader *This,
    WORD count, WORD *streams, WMT_STREAM_SELECTION *selections)
{
    TRACE_MSG("SR_SetStreamsSelected");
    ProxySyncReader *r = (ProxySyncReader *)This;
    for (WORD i = 0; i < count; i++) {
        WORD sn = streams[i];
        if (sn >= 1 && sn <= r->output_count)
            r->streams[sn - 1].selection = selections[i];
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetStreamSelected(IWMSyncReader *This,
    WORD wStreamNum, WMT_STREAM_SELECTION *pSel)
{
    TRACE_MSG("SR_GetStreamSelected");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (!pSel) return E_POINTER;
    if (wStreamNum < 1 || wStreamNum > r->output_count) return E_INVALIDARG;
    *pSel = r->streams[wStreamNum - 1].selection;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_SetReadStreamSamples(IWMSyncReader *This, WORD s, BOOL f) { return S_OK; }
static HRESULT STDMETHODCALLTYPE SR_GetReadStreamSamples(IWMSyncReader *This, WORD s, BOOL *f) {
    TRACE_MSG("SR_GetReadStreamSamples");
    if (f) *f = FALSE; return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetOutputSetting(IWMSyncReader *This,
    DWORD out, const WCHAR *name, WMT_ATTR_DATATYPE *type, BYTE *val, WORD *len)
{ return E_NOTIMPL; }

static HRESULT STDMETHODCALLTYPE SR_SetOutputSetting(IWMSyncReader *This,
    DWORD out, const WCHAR *name, WMT_ATTR_DATATYPE type, const BYTE *val, WORD len)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE SR_GetOutputCount(IWMSyncReader *This, DWORD *pCount) {
    TRACE_MSG("SR_GetOutputCount");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (!pCount) return E_POINTER;
    *pCount = r->output_count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetOutputProps(IWMSyncReader *This,
    DWORD out, IWMOutputMediaProps **ppProps)
{
    TRACE_MSG("SR_GetOutputProps");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (!ppProps) return E_POINTER;
    if (out >= r->output_count) return E_INVALIDARG;

    StreamInfo *si = &r->streams[out];
    WM_MEDIA_TYPE mt;
    union { VIDEOINFOHEADER vih; WAVEFORMATEX wfx; } fmt;

    if (si->is_video) fill_video_media_type(&mt, &fmt.vih, si);
    else              fill_audio_media_type(&mt, &fmt.wfx, si);

    ProxyOutputMediaProps *props = output_media_props_create(&mt);
    if (!props) return E_OUTOFMEMORY;
    *ppProps = (IWMOutputMediaProps *)props;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_SetOutputProps(IWMSyncReader *This,
    DWORD out, IWMOutputMediaProps *pProps)
{ return S_OK; }

static HRESULT STDMETHODCALLTYPE SR_GetOutputFormatCount(IWMSyncReader *This, DWORD out, DWORD *pCount) {
    TRACE_MSG("SR_GetOutputFormatCount");
    if (!pCount) return E_POINTER;
    *pCount = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetOutputFormat(IWMSyncReader *This,
    DWORD out, DWORD fmt_idx, IWMOutputMediaProps **ppProps)
{
    return SR_GetOutputProps(This, out, ppProps);
}

static HRESULT STDMETHODCALLTYPE SR_GetOutputNumberForStream(IWMSyncReader *This,
    WORD wStreamNum, DWORD *pdwOutputNum)
{
    TRACE_MSG("SR_GetOutputNumberForStream");
    if (!pdwOutputNum) return E_POINTER;
    *pdwOutputNum = (DWORD)(wStreamNum - 1);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetStreamNumberForOutput(IWMSyncReader *This,
    DWORD out, WORD *pwStreamNum)
{
    TRACE_MSG("SR_GetStreamNumberForOutput");
    if (!pwStreamNum) return E_POINTER;
    *pwStreamNum = (WORD)(out + 1);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetMaxOutputSampleSize(IWMSyncReader *This,
    DWORD out, DWORD *pcbMax)
{
    TRACE_MSG("SR_GetMaxOutputSampleSize");
    ProxySyncReader *r = (ProxySyncReader *)This;
    if (!pcbMax) return E_POINTER;
    if (out >= r->output_count) return E_INVALIDARG;
    StreamInfo *si = &r->streams[out];
    if (si->is_video)
        *pcbMax = si->out_width * si->out_height * (si->out_bpp / 8);
    else
        *pcbMax = si->out_sample_rate * si->out_channels * (si->out_bits_per_sample / 8);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SR_GetMaxStreamSampleSize(IWMSyncReader *This,
    WORD stream, DWORD *pcbMax)
{
    return SR_GetMaxOutputSampleSize(This, (DWORD)(stream - 1), pcbMax);
}

static HRESULT STDMETHODCALLTYPE SR_OpenStream(IWMSyncReader *This, IStream *pStream) {
    TRACE_MSG("SR_OpenStream");
    ProxySyncReader *r = (ProxySyncReader *)This;
    reader_close_internal(r);
    proxy_log("SyncReader::OpenStream(%p)", pStream);

    if (!pStream) return E_POINTER;

    pStream->lpVtbl->AddRef(pStream);
    r->source_stream = pStream;

    r->avio_buf = av_malloc(AVIO_BUF_SIZE);
    if (!r->avio_buf) return E_OUTOFMEMORY;

    r->avio_ctx = avio_alloc_context(
        r->avio_buf, AVIO_BUF_SIZE,
        0,              /* write_flag = 0 (read-only) */
        pStream,        /* opaque */
        avio_read_istream,
        NULL,           /* no write */
        avio_seek_istream);
    if (!r->avio_ctx) {
        av_freep(&r->avio_buf);
        return E_OUTOFMEMORY;
    }

    r->fmt_ctx = avformat_alloc_context();
    if (!r->fmt_ctx) return E_OUTOFMEMORY;
    r->fmt_ctx->pb = r->avio_ctx;

    int ret = avformat_open_input(&r->fmt_ctx, "stream.wmv", NULL, NULL);
    if (ret < 0) {
        proxy_log("  avformat_open_input (stream) failed: %d", ret);
        return E_FAIL;
    }

    return reader_open_internal(r);
}

/* ========== vtable ========== */

static IWMSyncReaderVtbl g_SyncReaderVtbl = {
    SR_QI, SR_AddRef, SR_Release,
    SR_Open, SR_Close,
    SR_SetRange, SR_SetRangeByFrame,
    SR_GetNextSample,
    SR_SetStreamsSelected, SR_GetStreamSelected,
    SR_SetReadStreamSamples, SR_GetReadStreamSamples,
    SR_GetOutputSetting, SR_SetOutputSetting,
    SR_GetOutputCount,
    SR_GetOutputProps, SR_SetOutputProps,
    SR_GetOutputFormatCount, SR_GetOutputFormat,
    SR_GetOutputNumberForStream, SR_GetStreamNumberForOutput,
    SR_GetMaxOutputSampleSize, SR_GetMaxStreamSampleSize,
    SR_OpenStream
};

HRESULT proxy_sync_reader_create(IWMSyncReader **ppReader) {
    if (!ppReader) return E_POINTER;
    ProxySyncReader *r = calloc(1, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    r->lpVtbl = &g_SyncReaderVtbl;
    r->ref_count = 1;
    r->range_duration = -1;

    /* Initialize embedded header info */
    r->header_info.lpVtbl = &g_HeaderInfoVtbl;
    r->header_info.owner = r;

    *ppReader = (IWMSyncReader *)r;
    proxy_log("Created ProxySyncReader @ %p", r);
    return S_OK;
}
