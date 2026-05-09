/*
 * MoviesAsfWriter — in-process WM ASF Writer replacement.
 *
 * Implements:
 *   IBaseFilter
 *   IFileSinkFilter        (game / CGB2 calls SetFileName)
 *   IConfigAsfWriter       (game / CGB2 hands us an IWMProfile)
 *   IPin + IMemInputPin    (one input pin per stream — initially video only)
 *
 * Internally uses libavformat (asf_stream muxer) + libavcodec (msmpeg4v2
 * for video) to write a real .wmv. Runs inside our process — no qasf.dll,
 * no qcap.dll, no wmvcore.dll required.
 *
 * Flow:
 *   1. Game CoCreates CLSID_WMAsfWriter. main.c hook returns mw_writer_create.
 *   2. Game / CGB2 QIs IConfigAsfWriter, calls ConfigureFilterUsingProfile.
 *      We capture the IWMProfile and read its IWMMediaProps to learn the
 *      stream count + per-stream WM_MEDIA_TYPE.
 *   3. Game / CGB2 QIs IFileSinkFilter, calls SetFileName(path, NULL).
 *   4. CGB2 calls IGraphBuilder::Connect, which lands in FakeGraph and is
 *      forwarded to source pin → our input pin's ReceiveConnection.
 *   5. Game calls IMediaControl::Run. We open the AVFormatContext lazily
 *      on the first received sample (so we know the negotiated mt).
 *   6. Per IMemInputPin::Receive: convert frame, encode, mux, write.
 *   7. On IBaseFilter::Stop: flush encoder, av_write_trailer, close.
 */
#include "asf_writer.h"
#include "log.h"
#include "trace.h"
#include "wm_types.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <wchar.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#ifndef VFW_E_ALREADY_CONNECTED
#define VFW_E_ALREADY_CONNECTED  ((HRESULT)0x80040204L)
#endif
#ifndef VFW_E_NO_ALLOCATOR
#define VFW_E_NO_ALLOCATOR       ((HRESULT)0x8004021AL)
#endif

/* ===== IFileSinkFilter vtable (5 slots) ===== */
typedef struct IFileSinkFilterVtbl_MW {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *SetFileName)(void*, LPCOLESTR, const AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *GetCurFile)(void*, LPOLESTR*, AM_MEDIA_TYPE*);
} IFileSinkFilterVtbl_MW;

/* ===== IConfigAsfWriter vtable (10 slots — methods inherited from IUnknown) =====
   Per qedit.idl: ConfigureFilterUsingProfileGuid, ConfigureFilterUsingProfile,
   ConfigureFilterUsingProfileId, GetCurrentProfile, GetCurrentProfileGuid,
   GetCurrentProfileId, SetIndexMode, GetIndexMode. */
typedef struct IConfigAsfWriterVtbl_MW {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG   (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *ConfigureFilterUsingProfileId)(void*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetCurrentProfileId)(void*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *ConfigureFilterUsingProfileGuid)(void*, REFGUID);
    HRESULT (STDMETHODCALLTYPE *GetCurrentProfileGuid)(void*, GUID*);
    HRESULT (STDMETHODCALLTYPE *ConfigureFilterUsingProfile)(void*, void* /*IWMProfile**/);
    HRESULT (STDMETHODCALLTYPE *GetCurrentProfile)(void*, void** /*IWMProfile***/);
    HRESULT (STDMETHODCALLTYPE *SetIndexMode)(void*, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetIndexMode)(void*, BOOL*);
} IConfigAsfWriterVtbl_MW;

/* ===== Forward decls ===== */
typedef struct MoviesAsfWriter MoviesAsfWriter;
typedef struct MwInputPin     MwInputPin;

/* ===== Input pin ===== */
struct MwInputPin {
    IPinVtbl_DS         *lpVtbl;            /* offset 0 */
    IMemInputPinVtbl_DS *lpMemInputPinVtbl; /* offset 4 */
    LONG                 ref_count;
    MoviesAsfWriter     *parent;            /* not AddRef'd — back-reference */
    IPin_DS             *peer;
    AM_MEDIA_TYPE        mt;
    BYTE                *mt_format_buf;
    BOOL                 connected;
    IMemAllocator_DS    *allocator;
    WCHAR                pin_id[32];
    int                  stream_index;      /* 0 = video, 1 = audio (later) */
};

/* ===== Filter ===== */
struct MoviesAsfWriter {
    IBaseFilterVtbl_DS      *lpBaseFilterVtbl;       /* offset 0 — primary identity */
    IFileSinkFilterVtbl_MW  *lpFileSinkFilterVtbl;   /* offset 4 */
    IConfigAsfWriterVtbl_MW *lpConfigAsfWriterVtbl;  /* offset 8 */
    LONG                     ref_count;

    IFilterGraph_DS         *graph;     /* not AddRef'd */
    WCHAR                    filter_name[128];
    FILTER_STATE             state;

    WCHAR                   *filename;
    void                    *profile;   /* IWMProfile* — opaque, we just store */

    MwInputPin              *input_pin;

    /* libavformat + libavcodec writer state */
    AVFormatContext         *fmt_ctx;
    AVStream                *vstream;
    AVCodecContext          *vcodec;
    AVFrame                 *frame_yuv;
    AVPacket                *pkt;
    int                      frame_w;
    int                      frame_h;
    int                      input_pix_fmt;        /* AVPixelFormat from negotiated mt */
    int                      input_stride;          /* bytes per source row */
    BOOL                     input_flip_v;          /* TRUE = bottom-up DIB, flip rows */
    AVRational               vframe_rate;           /* from the .prx avgtimeperframe */
    int64_t                  next_pts;
    BOOL                     writer_open;
    BOOL                     writer_finalized;
};

/* Macros to recover the filter from a sub-vtable pointer. */
#define WRITER_FROM_FS(p)  ((MoviesAsfWriter*)((BYTE*)(p) - offsetof(MoviesAsfWriter, lpFileSinkFilterVtbl)))
#define WRITER_FROM_CFG(p) ((MoviesAsfWriter*)((BYTE*)(p) - offsetof(MoviesAsfWriter, lpConfigAsfWriterVtbl)))
#define PIN_FROM_MEMIN(p)  ((MwInputPin*)((BYTE*)(p) - offsetof(MwInputPin, lpMemInputPinVtbl)))

/* ============================================================
 * IIDs we honour. The ones not already in wm_types.h get defined here.
 * ============================================================ */
static const GUID IID_IBaseFilter_local =
    {0x56A86895, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID IID_IMediaFilter_local =
    {0x56A86899, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID IID_IPersist_local =
    {0x0000010C, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID IID_IFileSinkFilter_local =
    {0xA2104830, 0x7C70, 0x11CF, {0x8B,0xCE,0x00,0xAA,0x00,0xA3,0xF1,0xA6}};
static const GUID IID_IConfigAsfWriter_local =
    {0x45086030, 0xF7E4, 0x486A, {0xB5,0x04,0x82,0x6B,0xB5,0x79,0x2A,0x3B}};
static const GUID IID_IPin_local =
    {0x56A86891, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID IID_IMemInputPin_local =
    {0x56A8689D, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* CLSID we identify as (so GetClassID returns the right thing) */
static const GUID CLSID_WMAsfWriter_local =
    {0x7C23220E, 0x55BB, 0x11D3, {0x8B,0x16,0x00,0xC0,0x4F,0xB6,0xBD,0x3D}};

/* Common uncompressed video subtypes the source might offer us. */
static const GUID MEDIASUBTYPE_RGB24_local =
    {0xE436EB7D, 0x524F, 0x11CE, {0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID MEDIASUBTYPE_RGB32_local =
    {0xE436EB7E, 0x524F, 0x11CE, {0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70}};
static const GUID MEDIASUBTYPE_YUY2_local =
    {0x32595559, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MEDIASUBTYPE_I420_local =
    {0x30323449, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

/* ============================================================
 * Forward decls of vtables (defined at bottom of file).
 * ============================================================ */
static IBaseFilterVtbl_DS       g_MwBaseFilterVtbl;
static IFileSinkFilterVtbl_MW   g_MwFileSinkFilterVtbl;
static IConfigAsfWriterVtbl_MW  g_MwConfigAsfWriterVtbl;
static IPinVtbl_DS              g_MwInputPinVtbl;
static IMemInputPinVtbl_DS      g_MwInputMemInputPinVtbl;

/* ============================================================
 * Helpers
 * ============================================================ */

/* Map a DirectShow uncompressed video subtype to an AVPixelFormat.
   Returns AV_PIX_FMT_NONE if we don't know the format. RGB32 from the
   game is BGRX (alpha byte is opaque/padding, never meaningful) — we
   tell sws BGR0 so it skips alpha-aware paths that produce striped U/V
   chroma in our setup. */
static int dshow_subtype_to_avpix(const GUID *subtype) {
    if (IsEqualGUID(subtype, &MEDIASUBTYPE_RGB24_local)) return AV_PIX_FMT_BGR24;
    if (IsEqualGUID(subtype, &MEDIASUBTYPE_RGB32_local)) return AV_PIX_FMT_BGR0;
    if (IsEqualGUID(subtype, &MEDIASUBTYPE_YUY2_local))  return AV_PIX_FMT_YUYV422;
    if (IsEqualGUID(subtype, &MEDIASUBTYPE_I420_local))  return AV_PIX_FMT_YUV420P;
    return AV_PIX_FMT_NONE;
}

static void mt_clear(AM_MEDIA_TYPE *mt, BYTE **owned_buf) {
    if (mt) memset(mt, 0, sizeof(*mt));
    if (owned_buf && *owned_buf) { free(*owned_buf); *owned_buf = NULL; }
}

static void mt_copy(AM_MEDIA_TYPE *dst, BYTE **dst_buf, const AM_MEDIA_TYPE *src) {
    *dst = *src;
    if (src->cbFormat > 0 && src->pbFormat) {
        *dst_buf = (BYTE*)malloc(src->cbFormat);
        if (*dst_buf) {
            memcpy(*dst_buf, src->pbFormat, src->cbFormat);
            dst->pbFormat = *dst_buf;
        } else {
            dst->cbFormat = 0;
            dst->pbFormat = NULL;
        }
    } else {
        dst->pbFormat = NULL;
        *dst_buf = NULL;
    }
    dst->pUnk = NULL;
}

/* ============================================================
 * Encoder lifecycle
 * ============================================================ */

static void writer_close_encoder(MoviesAsfWriter *w) {
    if (w->writer_finalized || !w->writer_open) goto cleanup;
    /* Flush encoder and write trailer. */
    if (w->vcodec) {
        avcodec_send_frame(w->vcodec, NULL);
        for (;;) {
            int ret = avcodec_receive_packet(w->vcodec, w->pkt);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
            if (ret < 0) break;
            w->pkt->stream_index = w->vstream->index;
            av_packet_rescale_ts(w->pkt, w->vcodec->time_base, w->vstream->time_base);
            av_interleaved_write_frame(w->fmt_ctx, w->pkt);
            av_packet_unref(w->pkt);
        }
    }
    if (w->fmt_ctx && (w->fmt_ctx->oformat->flags & AVFMT_NOFILE) == 0)
        av_write_trailer(w->fmt_ctx);
    w->writer_finalized = TRUE;

cleanup:
    if (w->frame_yuv) { av_frame_free(&w->frame_yuv); }
    if (w->pkt)       { av_packet_free(&w->pkt); }
    if (w->vcodec)    { avcodec_free_context(&w->vcodec); }
    if (w->fmt_ctx) {
        if (w->fmt_ctx->pb && (w->fmt_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&w->fmt_ctx->pb);
        }
        avformat_free_context(w->fmt_ctx);
        w->fmt_ctx = NULL;
    }
    w->writer_open = FALSE;
}

/* Open the FFmpeg writer using the negotiated input media type.
   Called lazily on the first IMemInputPin::Receive so we already know
   width/height/pixel format from ReceiveConnection. */
static HRESULT writer_open_encoder(MoviesAsfWriter *w) {
    if (w->writer_open) return S_OK;
    if (!w->filename || !w->input_pin || !w->input_pin->connected) {
        proxy_log("MwWriter: cannot open — filename=%p connected=%d",
                  w->filename, w->input_pin ? w->input_pin->connected : 0);
        return E_FAIL;
    }

    /* Convert the WCHAR filename to UTF-8 for libavformat. */
    int wlen = (int)wcslen(w->filename);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w->filename, wlen, NULL, 0, NULL, NULL);
    char *u8 = (char*)malloc((size_t)u8len + 1);
    if (!u8) return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_UTF8, 0, w->filename, wlen, u8, u8len, NULL, NULL);
    u8[u8len] = 0;
    proxy_log("MwWriter: opening %s", u8);

    /* "asf" is the actual muxer name (FFmpeg lists "asf,asf_stream" as
       aliases internally but only "asf" is valid for lookup). Passing
       the muxer name explicitly so the output format is unambiguous. */
    int rc = avformat_alloc_output_context2(&w->fmt_ctx, NULL, "asf", u8);
    if (rc < 0 || !w->fmt_ctx) {
        proxy_log("  avformat_alloc_output_context2 failed: %d", rc);
        free(u8);
        return E_FAIL;
    }

    /* WMV2 produced pink/cyan striping artifacts at our resolutions.
       msmpeg4v3 (FOURCC "WMV3", actually MS-MPEG4 v3 / DivX 3.11) is
       what the in-game profile asks for and is the closest practical
       match in libavcodec. Fall back order: msmpeg4v3 → wmv2 → wmv1. */
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MSMPEG4V3);
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_WMV2);
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_WMV1);
    if (!codec) {
        proxy_log("  no WMV/MSMPEG4 encoder available");
        free(u8);
        return E_FAIL;
    }
    proxy_log("  encoder: %s", codec->name);

    w->vstream = avformat_new_stream(w->fmt_ctx, NULL);
    if (!w->vstream) { free(u8); return E_FAIL; }

    w->vcodec = avcodec_alloc_context3(codec);
    if (!w->vcodec) { free(u8); return E_FAIL; }

    w->vcodec->codec_id   = codec->id;
    w->vcodec->codec_type = AVMEDIA_TYPE_VIDEO;
    w->vcodec->width      = w->frame_w;
    w->vcodec->height     = w->frame_h;
    w->vcodec->pix_fmt    = AV_PIX_FMT_YUV420P;
    /* Hardcoded 300kbps was producing severe codec artifacts on 768x432.
       Pick a bitrate proportional to resolution; the .prx profile hint
       can override later. */
    w->vcodec->bit_rate   = (int64_t)w->frame_w * w->frame_h * 3;  /* ≈3 bpp/s ⇒ ~1Mbps for 384x216, ~4Mbps for 768x432 */
    w->vcodec->gop_size   = 30;
    w->vcodec->max_b_frames = 0;
    w->vcodec->color_range  = AVCOL_RANGE_MPEG;
    w->vcodec->field_order  = AV_FIELD_PROGRESSIVE;
    if (w->vframe_rate.num == 0 || w->vframe_rate.den == 0) {
        w->vframe_rate.num = 30;
        w->vframe_rate.den = 1;
    }
    w->vcodec->time_base = (AVRational){ w->vframe_rate.den, w->vframe_rate.num };
    w->vcodec->framerate = w->vframe_rate;
    if (w->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        w->vcodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    rc = avcodec_open2(w->vcodec, codec, NULL);
    if (rc < 0) {
        proxy_log("  avcodec_open2 failed: %d", rc);
        free(u8);
        return E_FAIL;
    }

    rc = avcodec_parameters_from_context(w->vstream->codecpar, w->vcodec);
    if (rc < 0) { free(u8); return E_FAIL; }
    w->vstream->time_base = w->vcodec->time_base;

    rc = avio_open(&w->fmt_ctx->pb, u8, AVIO_FLAG_WRITE);
    free(u8);
    if (rc < 0) {
        proxy_log("  avio_open failed: %d", rc);
        return E_FAIL;
    }

    rc = avformat_write_header(w->fmt_ctx, NULL);
    if (rc < 0) {
        proxy_log("  avformat_write_header failed: %d", rc);
        return E_FAIL;
    }

    /* Set up the YUV scratch frame and sws converter. align=0 lets
       libav pick the natural alignment for the codec instead of forcing
       a 32-byte stride that may not line up with the codec's expectations. */
    w->frame_yuv = av_frame_alloc();
    w->frame_yuv->format = AV_PIX_FMT_YUV420P;
    w->frame_yuv->width  = w->frame_w;
    w->frame_yuv->height = w->frame_h;
    rc = av_frame_get_buffer(w->frame_yuv, 0);
    if (rc < 0) {
        proxy_log("  av_frame_get_buffer failed: %d", rc);
        return E_FAIL;
    }
    proxy_log("  frame linesize: Y=%d U=%d V=%d",
              w->frame_yuv->linesize[0], w->frame_yuv->linesize[1], w->frame_yuv->linesize[2]);
    w->pkt = av_packet_alloc();

    w->writer_open = TRUE;
    w->writer_finalized = FALSE;
    proxy_log("MwWriter: encoder open (%dx%d, src_pix=%d, %d/%d fps, codec=%s)",
              w->frame_w, w->frame_h, w->input_pix_fmt,
              w->vframe_rate.num, w->vframe_rate.den, codec->name);
    return S_OK;
}

/* Encode + mux one frame. Source data must be in input_pix_fmt with stride
   input_stride (bytes per row). pts is in 100ns units. */
static HRESULT writer_push_frame(MoviesAsfWriter *w, const BYTE *src, int src_stride, REFERENCE_TIME pts_100ns) {
    if (!w->writer_open) {
        HRESULT hr = writer_open_encoder(w);
        if (hr != S_OK) return hr;
    }

    const uint8_t *src_data[4] = { src, NULL, NULL, NULL };
    int src_lines[4]           = { src_stride, 0, 0, 0 };
    if (w->input_flip_v) {
        /* Bottom-up DIB: read starting from the last row, walking up. */
        src_data[0]  = src + (size_t)src_stride * (w->frame_h - 1);
        src_lines[0] = -src_stride;
    }

    /* Hand-rolled BGRX → YUV420P (BT.601 limited range) — sws_scale was
       producing structured garbage (binary 0/255 in U/V, periodic across
       rows) regardless of source format. Manual conversion guarantees
       correctness. Uses 8.8 fixed-point math; chroma is 2x2-averaged. */
    {
        const int W = w->frame_w;
        const int H = w->frame_h;
        const int bpp = (w->input_pix_fmt == AV_PIX_FMT_BGR24) ? 3 : 4;
        uint8_t *Yp = w->frame_yuv->data[0];
        uint8_t *Up = w->frame_yuv->data[1];
        uint8_t *Vp = w->frame_yuv->data[2];
        const int Yls = w->frame_yuv->linesize[0];
        const int Uls = w->frame_yuv->linesize[1];
        const int Vls = w->frame_yuv->linesize[2];

        for (int y = 0; y < H; y++) {
            const uint8_t *srow0 = src_data[0] + (intptr_t)y * src_lines[0];
            uint8_t *yrow = Yp + y * Yls;
            for (int x = 0; x < W; x++) {
                int B = srow0[x * bpp + 0];
                int G = srow0[x * bpp + 1];
                int R = srow0[x * bpp + 2];
                int Yv = (66*R + 129*G + 25*B + 128) >> 8;     /* +16 below */
                yrow[x] = (uint8_t)(Yv + 16);
            }
        }
        /* Chroma: average each 2x2 block, write one U/V per block. */
        for (int y = 0; y < H; y += 2) {
            const uint8_t *srow0 = src_data[0] + (intptr_t)y       * src_lines[0];
            const uint8_t *srow1 = src_data[0] + (intptr_t)(y + 1) * src_lines[0];
            uint8_t *urow = Up + (y / 2) * Uls;
            uint8_t *vrow = Vp + (y / 2) * Vls;
            for (int x = 0; x < W; x += 2) {
                int B = (srow0[x*bpp+0] + srow0[(x+1)*bpp+0] +
                         srow1[x*bpp+0] + srow1[(x+1)*bpp+0] + 2) >> 2;
                int G = (srow0[x*bpp+1] + srow0[(x+1)*bpp+1] +
                         srow1[x*bpp+1] + srow1[(x+1)*bpp+1] + 2) >> 2;
                int R = (srow0[x*bpp+2] + srow0[(x+1)*bpp+2] +
                         srow1[x*bpp+2] + srow1[(x+1)*bpp+2] + 2) >> 2;
                int Uv = ((-38*R - 74*G + 112*B + 128) >> 8) + 128;
                int Vv = (( 112*R - 94*G - 18*B + 128) >> 8) + 128;
                if (Uv < 0) Uv = 0; else if (Uv > 255) Uv = 255;
                if (Vv < 0) Vv = 0; else if (Vv > 255) Vv = 255;
                urow[x / 2] = (uint8_t)Uv;
                vrow[x / 2] = (uint8_t)Vv;
            }
        }
    }

    int64_t pts;
    if (w->vframe_rate.num > 0)
        pts = w->next_pts++;
    else
        pts = pts_100ns / (10000000 / 30);
    w->frame_yuv->pts = pts;

    int rc = avcodec_send_frame(w->vcodec, w->frame_yuv);
    if (rc < 0) {
        proxy_log("  send_frame: %d", rc);
        return E_FAIL;
    }
    for (;;) {
        rc = avcodec_receive_packet(w->vcodec, w->pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) { proxy_log("  receive_packet: %d", rc); return E_FAIL; }
        w->pkt->stream_index = w->vstream->index;
        av_packet_rescale_ts(w->pkt, w->vcodec->time_base, w->vstream->time_base);
        av_interleaved_write_frame(w->fmt_ctx, w->pkt);
        av_packet_unref(w->pkt);
    }
    return S_OK;
}

/* ============================================================
 * IBaseFilter on MoviesAsfWriter
 * ============================================================ */

static HRESULT STDMETHODCALLTYPE BF_QI(IBaseFilter_DS *This, REFIID riid, void **ppv) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IBaseFilter_local)
        || IsEqualGUID(riid, &IID_IMediaFilter_local)
        || IsEqualGUID(riid, &IID_IPersist_local))
    {
        *ppv = This;
        InterlockedIncrement(&w->ref_count);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IFileSinkFilter_local)) {
        *ppv = &w->lpFileSinkFilterVtbl;
        InterlockedIncrement(&w->ref_count);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IConfigAsfWriter_local)) {
        *ppv = &w->lpConfigAsfWriterVtbl;
        InterlockedIncrement(&w->ref_count);
        return S_OK;
    }
    proxy_log("MwWriter::QI E_NOINTERFACE for {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        riid->Data1, riid->Data2, riid->Data3,
        riid->Data4[0], riid->Data4[1], riid->Data4[2], riid->Data4[3],
        riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE BF_AddRef(IBaseFilter_DS *This) {
    return InterlockedIncrement(&((MoviesAsfWriter*)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE BF_Release(IBaseFilter_DS *This) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    LONG ref = InterlockedDecrement(&w->ref_count);
    if (ref == 0) {
        proxy_log("MwWriter destroyed");
        writer_close_encoder(w);
        if (w->input_pin) {
            mt_clear(&w->input_pin->mt, &w->input_pin->mt_format_buf);
            free(w->input_pin);
        }
        free(w->filename);
        free(w);
    }
    return ref;
}
static HRESULT STDMETHODCALLTYPE BF_GetClassID(IBaseFilter_DS *This, CLSID *pClsid) {
    (void)This;
    if (pClsid) *pClsid = CLSID_WMAsfWriter_local;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_Stop(IBaseFilter_DS *This) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    proxy_log("MwWriter::Stop");
    if (w->state != State_Stopped) {
        writer_close_encoder(w);
    }
    w->state = State_Stopped;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_Pause(IBaseFilter_DS *This) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    proxy_log("MwWriter::Pause");
    w->state = State_Paused;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_Run(IBaseFilter_DS *This, REFERENCE_TIME tStart) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    proxy_log("MwWriter::Run(t=%lld)", (long long)tStart);
    w->state = State_Running;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_GetState(IBaseFilter_DS *This, DWORD ms, FILTER_STATE *pState) {
    (void)ms;
    if (pState) *pState = ((MoviesAsfWriter*)This)->state;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_SetSyncSource(IBaseFilter_DS *This, IReferenceClock_DS *pClock) {
    (void)This; (void)pClock; return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_GetSyncSource(IBaseFilter_DS *This, IReferenceClock_DS **ppClock) {
    (void)This;
    if (ppClock) *ppClock = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_EnumPins(IBaseFilter_DS *This, IEnumPins_DS **ppEnum);  /* defined below */
static HRESULT STDMETHODCALLTYPE BF_FindPin(IBaseFilter_DS *This, LPCWSTR Id, IPin_DS **ppPin) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    if (!ppPin) return E_POINTER;
    *ppPin = NULL;
    if (Id && w->input_pin && wcscmp(Id, w->input_pin->pin_id) == 0) {
        *ppPin = (IPin_DS*)w->input_pin;
        InterlockedIncrement(&w->input_pin->ref_count);
        return S_OK;
    }
    return VFW_E_NOT_FOUND;
}
static HRESULT STDMETHODCALLTYPE BF_QueryFilterInfo(IBaseFilter_DS *This, FILTER_INFO *pInfo) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    if (!pInfo) return E_POINTER;
    wcscpy(pInfo->achName, w->filter_name);
    pInfo->pGraph = w->graph;
    if (w->graph) ((IUnknown*)w->graph)->lpVtbl->AddRef((IUnknown*)w->graph);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_JoinFilterGraph(IBaseFilter_DS *This, IFilterGraph_DS *pGraph, LPCWSTR pName) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    w->graph = pGraph;
    if (pName) {
        wcsncpy(w->filter_name, pName, 127);
        w->filter_name[127] = 0;
    }
    proxy_log("MwWriter::JoinFilterGraph(%ls)", pName ? pName : L"(null)");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_QueryVendorInfo(IBaseFilter_DS *This, LPWSTR *pVendor) {
    (void)This;
    if (pVendor) *pVendor = NULL;
    return E_NOTIMPL;
}

/* IEnumPins for our single input pin. */
typedef struct {
    IEnumPinsVtbl_DS *lpVtbl;
    LONG              ref;
    MoviesAsfWriter  *parent;
    int               cursor;
} MwEnumPins;
static IEnumPinsVtbl_DS g_MwEnumPinsVtbl;
static const GUID IID_IEnumPins_local =
    {0x56A86892, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

static HRESULT STDMETHODCALLTYPE EP_QI(IEnumPins_DS *This, REFIID riid, void **ppv) {
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IEnumPins_local)) {
        *ppv = This; InterlockedIncrement(&((MwEnumPins*)This)->ref); return S_OK;
    }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EP_AddRef(IEnumPins_DS *This) {
    return InterlockedIncrement(&((MwEnumPins*)This)->ref);
}
static ULONG STDMETHODCALLTYPE EP_Release(IEnumPins_DS *This) {
    MwEnumPins *e = (MwEnumPins*)This;
    LONG r = InterlockedDecrement(&e->ref);
    if (r == 0) { BF_Release((IBaseFilter_DS*)e->parent); free(e); }
    return r;
}
static HRESULT STDMETHODCALLTYPE EP_Next(IEnumPins_DS *This, ULONG cReq, IPin_DS **out, ULONG *fetched) {
    MwEnumPins *e = (MwEnumPins*)This;
    ULONG n = 0;
    while (n < cReq && e->cursor == 0 && e->parent->input_pin) {
        out[n] = (IPin_DS*)e->parent->input_pin;
        InterlockedIncrement(&e->parent->input_pin->ref_count);
        n++;
        e->cursor++;
    }
    if (fetched) *fetched = n;
    return (n == cReq) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EP_Skip(IEnumPins_DS *This, ULONG c) {
    MwEnumPins *e = (MwEnumPins*)This;
    e->cursor += c; if (e->cursor > 1) e->cursor = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE EP_Reset(IEnumPins_DS *This) {
    ((MwEnumPins*)This)->cursor = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE EP_Clone(IEnumPins_DS *This, IEnumPins_DS **ppEnum) {
    MwEnumPins *src = (MwEnumPins*)This;
    MwEnumPins *dst = (MwEnumPins*)calloc(1, sizeof(*dst));
    if (!dst) { *ppEnum = NULL; return E_OUTOFMEMORY; }
    dst->lpVtbl = &g_MwEnumPinsVtbl;
    dst->ref = 1;
    dst->parent = src->parent;
    dst->cursor = src->cursor;
    BF_AddRef((IBaseFilter_DS*)dst->parent);
    *ppEnum = (IEnumPins_DS*)dst;
    return S_OK;
}
static IEnumPinsVtbl_DS g_MwEnumPinsVtbl = {
    EP_QI, EP_AddRef, EP_Release, EP_Next, EP_Skip, EP_Reset, EP_Clone
};

static HRESULT STDMETHODCALLTYPE BF_EnumPins(IBaseFilter_DS *This, IEnumPins_DS **ppEnum) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    if (!ppEnum) return E_POINTER;
    MwEnumPins *e = (MwEnumPins*)calloc(1, sizeof(*e));
    if (!e) { *ppEnum = NULL; return E_OUTOFMEMORY; }
    e->lpVtbl = &g_MwEnumPinsVtbl;
    e->ref = 1;
    e->parent = w;
    e->cursor = 0;
    BF_AddRef(This);
    *ppEnum = (IEnumPins_DS*)e;
    return S_OK;
}

static IBaseFilterVtbl_DS g_MwBaseFilterVtbl = {
    BF_QI, BF_AddRef, BF_Release, BF_GetClassID,
    BF_Stop, BF_Pause, BF_Run, BF_GetState,
    BF_SetSyncSource, BF_GetSyncSource,
    BF_EnumPins, BF_FindPin,
    BF_QueryFilterInfo, BF_JoinFilterGraph, BF_QueryVendorInfo
};

/* ============================================================
 * IFileSinkFilter
 * ============================================================ */

static HRESULT STDMETHODCALLTYPE FS_QI(void *t, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)WRITER_FROM_FS(t), riid, ppv);
}
static ULONG STDMETHODCALLTYPE FS_AddRef(void *t) {
    return BF_AddRef((IBaseFilter_DS*)WRITER_FROM_FS(t));
}
static ULONG STDMETHODCALLTYPE FS_Release(void *t) {
    return BF_Release((IBaseFilter_DS*)WRITER_FROM_FS(t));
}
static HRESULT STDMETHODCALLTYPE FS_SetFileName(void *t, LPCOLESTR pszName, const AM_MEDIA_TYPE *pmt) {
    MoviesAsfWriter *w = WRITER_FROM_FS(t);
    (void)pmt;
    proxy_log("MwWriter::SetFileName(%ls)", pszName ? pszName : L"(null)");
    free(w->filename);
    w->filename = NULL;
    if (pszName) {
        size_t n = wcslen(pszName);
        w->filename = (WCHAR*)malloc((n + 1) * sizeof(WCHAR));
        if (!w->filename) return E_OUTOFMEMORY;
        memcpy(w->filename, pszName, (n + 1) * sizeof(WCHAR));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE FS_GetCurFile(void *t, LPOLESTR *ppszName, AM_MEDIA_TYPE *pmt) {
    MoviesAsfWriter *w = WRITER_FROM_FS(t);
    if (pmt) memset(pmt, 0, sizeof(*pmt));
    if (!ppszName) return E_POINTER;
    if (!w->filename) { *ppszName = NULL; return S_OK; }
    size_t bytes = (wcslen(w->filename) + 1) * sizeof(WCHAR);
    *ppszName = (LPOLESTR)CoTaskMemAlloc(bytes);
    if (!*ppszName) return E_OUTOFMEMORY;
    memcpy(*ppszName, w->filename, bytes);
    return S_OK;
}
static IFileSinkFilterVtbl_MW g_MwFileSinkFilterVtbl = {
    FS_QI, FS_AddRef, FS_Release, FS_SetFileName, FS_GetCurFile
};

/* ============================================================
 * IConfigAsfWriter — store the IWMProfile for later inspection
 * ============================================================ */

static HRESULT STDMETHODCALLTYPE CFG_QI(void *t, REFIID riid, void **ppv) {
    return BF_QI((IBaseFilter_DS*)WRITER_FROM_CFG(t), riid, ppv);
}
static ULONG STDMETHODCALLTYPE CFG_AddRef(void *t) {
    return BF_AddRef((IBaseFilter_DS*)WRITER_FROM_CFG(t));
}
static ULONG STDMETHODCALLTYPE CFG_Release(void *t) {
    return BF_Release((IBaseFilter_DS*)WRITER_FROM_CFG(t));
}
static HRESULT STDMETHODCALLTYPE CFG_ConfigureFilterUsingProfileId(void *t, DWORD id) {
    (void)t; proxy_log("MwWriter::ConfigureFilterUsingProfileId(%lu)", (unsigned long)id);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_GetCurrentProfileId(void *t, DWORD *pId) {
    (void)t; if (pId) *pId = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_ConfigureFilterUsingProfileGuid(void *t, REFGUID g) {
    (void)t; (void)g; return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_GetCurrentProfileGuid(void *t, GUID *pg) {
    (void)t; if (pg) memset(pg, 0, sizeof(*pg)); return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_ConfigureFilterUsingProfile(void *t, void *pProfile) {
    MoviesAsfWriter *w = WRITER_FROM_CFG(t);
    proxy_log("MwWriter::ConfigureFilterUsingProfile(%p)", pProfile);
    w->profile = pProfile;
    if (pProfile) {
        ((IUnknown*)pProfile)->lpVtbl->AddRef((IUnknown*)pProfile);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_GetCurrentProfile(void *t, void **pp) {
    MoviesAsfWriter *w = WRITER_FROM_CFG(t);
    if (!pp) return E_POINTER;
    *pp = w->profile;
    if (w->profile) ((IUnknown*)w->profile)->lpVtbl->AddRef((IUnknown*)w->profile);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE CFG_SetIndexMode(void *t, BOOL b) { (void)t; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE CFG_GetIndexMode(void *t, BOOL *pb) { (void)t; if (pb) *pb = FALSE; return S_OK; }

static IConfigAsfWriterVtbl_MW g_MwConfigAsfWriterVtbl = {
    CFG_QI, CFG_AddRef, CFG_Release,
    CFG_ConfigureFilterUsingProfileId, CFG_GetCurrentProfileId,
    CFG_ConfigureFilterUsingProfileGuid, CFG_GetCurrentProfileGuid,
    CFG_ConfigureFilterUsingProfile, CFG_GetCurrentProfile,
    CFG_SetIndexMode, CFG_GetIndexMode
};

/* ============================================================
 * Input pin (IPin + IMemInputPin)
 * ============================================================ */

static HRESULT STDMETHODCALLTYPE Pin_QI(IPin_DS *This, REFIID riid, void **ppv) {
    MwInputPin *p = (MwInputPin*)This;
    if (!ppv) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IPin_local)) {
        *ppv = This; InterlockedIncrement(&p->ref_count); return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IMemInputPin_local)) {
        *ppv = &p->lpMemInputPinVtbl; InterlockedIncrement(&p->ref_count); return S_OK;
    }
    proxy_log("MwInputPin::QI E_NOINTERFACE for {%08lX-%04X-%04X}", riid->Data1, riid->Data2, riid->Data3);
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Pin_AddRef(IPin_DS *This) {
    return InterlockedIncrement(&((MwInputPin*)This)->ref_count);
}
static ULONG STDMETHODCALLTYPE Pin_Release(IPin_DS *This) {
    return InterlockedDecrement(&((MwInputPin*)This)->ref_count);
}
static HRESULT STDMETHODCALLTYPE Pin_Connect(IPin_DS *This, IPin_DS *p, const AM_MEDIA_TYPE *mt) {
    (void)This; (void)p; (void)mt;
    /* Input pins don't get Connect; they get ReceiveConnection. */
    return E_UNEXPECTED;
}
static HRESULT STDMETHODCALLTYPE Pin_ReceiveConnection(IPin_DS *This, IPin_DS *peer, const AM_MEDIA_TYPE *pmt) {
    MwInputPin *p = (MwInputPin*)This;
    if (!peer || !pmt) return E_POINTER;
    if (p->connected) return VFW_E_ALREADY_CONNECTED;

    proxy_log("MwInputPin::ReceiveConnection major={%08lX} sub={%08lX} cbFormat=%lu",
              pmt->majortype.Data1, pmt->subtype.Data1, (unsigned long)pmt->cbFormat);

    int avpix = dshow_subtype_to_avpix(&pmt->subtype);
    if (avpix == AV_PIX_FMT_NONE) {
        proxy_log("  unknown subtype — refusing");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    /* Pull width/height/stride/frame rate from the format buffer. */
    int w = 0, h = 0, stride = 0;
    BOOL flip_v = FALSE;
    int bicompression = 0, bibitcount = 0;
    AVRational fr = {30, 1};
    if (pmt->cbFormat >= sizeof(VIDEOINFOHEADER) && pmt->pbFormat) {
        const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER*)pmt->pbFormat;
        w = vih->bmiHeader.biWidth;
        /* DIB convention: positive biHeight = bottom-up, negative = top-down.
           Bottom-up needs vertical flip when feeding sws. */
        if (vih->bmiHeader.biHeight < 0) {
            h = -vih->bmiHeader.biHeight;
            flip_v = FALSE;
        } else {
            h = vih->bmiHeader.biHeight;
            flip_v = TRUE;
        }
        bicompression = vih->bmiHeader.biCompression;
        bibitcount    = vih->bmiHeader.biBitCount;
        stride = ((w * bibitcount / 8) + 3) & ~3;
        if (vih->AvgTimePerFrame > 0) {
            fr.num = 10000000;
            fr.den = (int)vih->AvgTimePerFrame;
        }
    }
    if (w <= 0 || h <= 0) {
        proxy_log("  invalid dimensions w=%d h=%d", w, h);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    /* Cache the type and the frame parameters. */
    mt_clear(&p->mt, &p->mt_format_buf);
    mt_copy(&p->mt, &p->mt_format_buf, pmt);

    p->parent->frame_w        = w;
    p->parent->frame_h        = h;
    p->parent->input_pix_fmt  = avpix;
    p->parent->input_stride   = stride;
    p->parent->input_flip_v   = flip_v;
    p->parent->vframe_rate    = fr;
    p->parent->next_pts       = 0;

    p->peer = peer;
    peer->lpVtbl->AddRef(peer);
    p->connected = TRUE;
    proxy_log("  accepted: %dx%d stride=%d avpix=%d fps=%d/%d flip_v=%d biCompression=0x%08X biBitCount=%d",
              w, h, stride, avpix, fr.num, fr.den, flip_v, bicompression, bibitcount);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_Disconnect(IPin_DS *This) {
    MwInputPin *p = (MwInputPin*)This;
    if (!p->connected) return S_FALSE;
    if (p->peer) { p->peer->lpVtbl->Release(p->peer); p->peer = NULL; }
    if (p->allocator) { p->allocator->lpVtbl->Release(p->allocator); p->allocator = NULL; }
    mt_clear(&p->mt, &p->mt_format_buf);
    p->connected = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_ConnectedTo(IPin_DS *This, IPin_DS **ppPin) {
    MwInputPin *p = (MwInputPin*)This;
    if (!ppPin) return E_POINTER;
    if (!p->connected) { *ppPin = NULL; return VFW_E_NOT_CONNECTED; }
    *ppPin = p->peer;
    if (p->peer) p->peer->lpVtbl->AddRef(p->peer);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_ConnectionMediaType(IPin_DS *This, AM_MEDIA_TYPE *pmt) {
    MwInputPin *p = (MwInputPin*)This;
    if (!pmt) return E_POINTER;
    if (!p->connected) { memset(pmt, 0, sizeof(*pmt)); return VFW_E_NOT_CONNECTED; }
    BYTE *tmp = NULL; mt_copy(pmt, &tmp, &p->mt);
    /* The caller-supplied AM_MEDIA_TYPE uses CoTaskMemAlloc semantics for
       pbFormat — but mt_copy used malloc. Re-allocate via CoTaskMemAlloc. */
    if (tmp) {
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(p->mt.cbFormat);
        if (pmt->pbFormat) memcpy(pmt->pbFormat, tmp, p->mt.cbFormat);
        free(tmp);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_QueryPinInfo(IPin_DS *This, PIN_INFO *pInfo) {
    MwInputPin *p = (MwInputPin*)This;
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = (IBaseFilter_DS*)p->parent;
    BF_AddRef((IBaseFilter_DS*)p->parent);
    pInfo->dir = PINDIR_INPUT;
    wcscpy(pInfo->achName, p->pin_id);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_QueryDirection(IPin_DS *This, PIN_DIRECTION *pDir) {
    (void)This;
    if (pDir) *pDir = PINDIR_INPUT;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_QueryId(IPin_DS *This, LPWSTR *pId) {
    MwInputPin *p = (MwInputPin*)This;
    if (!pId) return E_POINTER;
    size_t bytes = (wcslen(p->pin_id) + 1) * sizeof(WCHAR);
    *pId = (LPWSTR)CoTaskMemAlloc(bytes);
    if (!*pId) return E_OUTOFMEMORY;
    memcpy(*pId, p->pin_id, bytes);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_QueryAccept(IPin_DS *This, const AM_MEDIA_TYPE *pmt) {
    (void)This;
    if (!pmt) return E_POINTER;
    if (!IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Video)) return S_FALSE;
    return (dshow_subtype_to_avpix(&pmt->subtype) != AV_PIX_FMT_NONE) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE Pin_EnumMediaTypes(IPin_DS *This, IEnumMediaTypes_DS **ppEnum) {
    /* Input pins don't typically need to enumerate types — the upstream
       output pin offers them. Return E_NOTIMPL to make negotiation flow
       in the standard direction. */
    (void)This;
    if (ppEnum) *ppEnum = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Pin_QueryInternalConnections(IPin_DS *This, IPin_DS **p, ULONG *n) {
    (void)This; (void)p; (void)n; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Pin_EndOfStream(IPin_DS *This) {
    MwInputPin *p = (MwInputPin*)This;
    proxy_log("MwInputPin::EndOfStream");
    /* Flush + finalize on EOS so the file has a trailer even if the game
       doesn't call Stop in time. */
    writer_close_encoder(p->parent);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_BeginFlush(IPin_DS *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE Pin_EndFlush(IPin_DS *This)   { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE Pin_NewSegment(IPin_DS *This, REFERENCE_TIME s, REFERENCE_TIME e, double r) {
    (void)This; (void)s; (void)e; (void)r; return S_OK;
}

static IPinVtbl_DS g_MwInputPinVtbl = {
    Pin_QI, Pin_AddRef, Pin_Release,
    Pin_Connect, Pin_ReceiveConnection, Pin_Disconnect,
    Pin_ConnectedTo, Pin_ConnectionMediaType,
    Pin_QueryPinInfo, Pin_QueryDirection, Pin_QueryId,
    Pin_QueryAccept, Pin_EnumMediaTypes, Pin_QueryInternalConnections,
    Pin_EndOfStream, Pin_BeginFlush, Pin_EndFlush, Pin_NewSegment
};

/* IMemInputPin: Receive is where samples actually arrive. */
static HRESULT STDMETHODCALLTYPE MIP_QI(IMemInputPin_DS *This, REFIID riid, void **ppv) {
    MwInputPin *p = PIN_FROM_MEMIN(This);
    return Pin_QI((IPin_DS*)p, riid, ppv);
}
static ULONG STDMETHODCALLTYPE MIP_AddRef(IMemInputPin_DS *This) {
    return Pin_AddRef((IPin_DS*)PIN_FROM_MEMIN(This));
}
static ULONG STDMETHODCALLTYPE MIP_Release(IMemInputPin_DS *This) {
    return Pin_Release((IPin_DS*)PIN_FROM_MEMIN(This));
}
static HRESULT STDMETHODCALLTYPE MIP_GetAllocator(IMemInputPin_DS *This, IMemAllocator_DS **ppAlloc) {
    (void)This;
    /* We don't supply an allocator — the upstream filter is welcome to
       provide its own via NotifyAllocator. */
    if (ppAlloc) *ppAlloc = NULL;
    return VFW_E_NO_ALLOCATOR;
}
static HRESULT STDMETHODCALLTYPE MIP_NotifyAllocator(IMemInputPin_DS *This, IMemAllocator_DS *pAlloc, BOOL bReadOnly) {
    MwInputPin *p = PIN_FROM_MEMIN(This);
    (void)bReadOnly;
    if (p->allocator) p->allocator->lpVtbl->Release(p->allocator);
    p->allocator = pAlloc;
    if (pAlloc) pAlloc->lpVtbl->AddRef(pAlloc);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE MIP_GetAllocatorRequirements(IMemInputPin_DS *This, ALLOCATOR_PROPERTIES *pProps) {
    (void)This;
    if (pProps) memset(pProps, 0, sizeof(*pProps));
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE MIP_Receive(IMemInputPin_DS *This, IMediaSample_DS *pSample) {
    MwInputPin *p = PIN_FROM_MEMIN(This);
    if (!pSample || !p->connected) return E_FAIL;

    BYTE *buffer = NULL;
    pSample->lpVtbl->GetPointer(pSample, &buffer);
    long actual = pSample->lpVtbl->GetActualDataLength(pSample);
    REFERENCE_TIME tStart = 0, tEnd = 0;
    pSample->lpVtbl->GetTime(pSample, &tStart, &tEnd);

    if (!buffer || actual <= 0) return S_OK;

    /* Stride computed at ReceiveConnection (DWORD-aligned width * bpp/8).
       Some sources pack differently; if biBitCount stride disagrees with
       actual_data_length / height we trust the derived value. */
    int expected_stride = p->parent->input_stride;
    int derived_stride  = actual / p->parent->frame_h;
    int stride = (derived_stride > 0 && derived_stride != expected_stride)
                 ? derived_stride : expected_stride;
    return writer_push_frame(p->parent, buffer, stride, tStart);
}
static HRESULT STDMETHODCALLTYPE MIP_ReceiveMultiple(IMemInputPin_DS *This, IMediaSample_DS **samples, long n, long *processed) {
    long i;
    HRESULT hr = S_OK;
    for (i = 0; i < n; i++) {
        hr = MIP_Receive(This, samples[i]);
        if (hr != S_OK) break;
    }
    if (processed) *processed = i;
    return hr;
}
static HRESULT STDMETHODCALLTYPE MIP_ReceiveCanBlock(IMemInputPin_DS *This) {
    (void)This;
    return S_FALSE;   /* never blocks — encode is in-thread */
}
static IMemInputPinVtbl_DS g_MwInputMemInputPinVtbl = {
    MIP_QI, MIP_AddRef, MIP_Release,
    MIP_GetAllocator, MIP_NotifyAllocator, MIP_GetAllocatorRequirements,
    MIP_Receive, MIP_ReceiveMultiple, MIP_ReceiveCanBlock
};

/* ============================================================
 * Factory
 * ============================================================ */

HRESULT mw_writer_create(IBaseFilter_DS **ppFilter) {
    if (!ppFilter) return E_POINTER;

    MoviesAsfWriter *w = (MoviesAsfWriter*)calloc(1, sizeof(*w));
    if (!w) return E_OUTOFMEMORY;

    w->lpBaseFilterVtbl       = &g_MwBaseFilterVtbl;
    w->lpFileSinkFilterVtbl   = &g_MwFileSinkFilterVtbl;
    w->lpConfigAsfWriterVtbl  = &g_MwConfigAsfWriterVtbl;
    w->ref_count              = 1;
    w->state                  = State_Stopped;
    wcscpy(w->filter_name, L"WM ASF Writer");

    MwInputPin *pin = (MwInputPin*)calloc(1, sizeof(*pin));
    if (!pin) { free(w); return E_OUTOFMEMORY; }
    pin->lpVtbl              = &g_MwInputPinVtbl;
    pin->lpMemInputPinVtbl   = &g_MwInputMemInputPinVtbl;
    pin->ref_count           = 1;
    pin->parent              = w;
    pin->stream_index        = 0;
    wcscpy(pin->pin_id, L"VideoInput01");
    w->input_pin = pin;

    *ppFilter = (IBaseFilter_DS*)w;
    proxy_log("MwWriter created @ %p (replacing CLSID_WMAsfWriter)", w);
    return S_OK;
}
