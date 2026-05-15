/*
 * MoviesAsfWriter — in-process WM ASF Writer replacement that produces MP4.
 *
 * Despite the class identity it advertises (CLSID_WMAsfWriter), the file
 * we actually write is MP4/H.264/AAC, not ASF/WMV. The game's "Render
 * Movie" UI doesn't care about the container; it cares about being able
 * to CoCreate the WMAsfWriter CLSID and hand it an IWMProfile.
 *
 * Implements:
 *   IBaseFilter
 *   IFileSinkFilter        (game / CGB2 calls SetFileName — we rewrite .wmv → .mp4)
 *   IConfigAsfWriter       (game / CGB2 hands us an IWMProfile — we read its bitrate)
 *   IPin + IMemInputPin    (video pin: real samples;
 *                           audio pin: accepts negotiation, drops samples —
 *                                      audio is read directly from the
 *                                      pre-pass TMP wav instead)
 *
 * Pipeline:
 *   Video: BGRX/BGR24 from the game → hand-rolled BT.601 BGR→YUV420P →
 *          libopenh264 (BSD-2-Clause, Cisco patent royalty coverage).
 *   Audio: We DO NOT use the in-graph audio path. The game's "Audio Enc"
 *          system filter produces MSADPCM that we cannot reliably mux
 *          without re-implementing the broken Wave Dest finalize. Instead,
 *          we read raw PCM directly from the TMP wav written by the
 *          pre-pass (CAviSyst global at *(void**)0x010bab18 + 0x210),
 *          ignoring its broken data_chunk_size (we trust file size), and
 *          transcode to AAC (FFmpeg native, LGPL).
 *
 * Container is mp4 via libavformat's mov muxer (LGPL). NO --enable-gpl.
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

typedef enum { PIN_KIND_VIDEO = 0, PIN_KIND_AUDIO = 1 } PinKind;

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
    PinKind              kind;
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

    MwInputPin              *video_pin;
    MwInputPin              *audio_pin;       /* NULL until mw_writer_create — game may or may not connect it */

    /* libavformat shared state */
    AVFormatContext         *fmt_ctx;

    /* Video encoder state */
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

    /* Audio state (added in #12). Two paths depending on what the
       upstream "Audio Enc" filter offers:
         PCM      → set up wmav2 encoder, transcode S16 → FLTP → WMA2.
         MSADPCM  → no encoder; mux raw blocks straight into the ASF
                    via AV_CODEC_ID_ADPCM_MS (the format the game's
                    audio compressor actually produces by default).
       audio_codec_id discriminates. Stays AV_CODEC_ID_NONE until the
       audio pin negotiates a connection. */
    AVStream                *astream;
    AVCodecContext          *acodec;            /* PCM path only */
    AVFrame                 *frame_pcm;         /* PCM path only */
    AVPacket                *apkt;
    enum AVCodecID           audio_codec_id;    /* PCM_S16LE or ADPCM_MS */
    int                      audio_sample_rate; /* from WAVEFORMATEX */
    int                      audio_channels;
    int                      audio_bits_per_sample;
    int                      audio_block_align; /* bytes per ADPCM block / PCM frame */
    int                      audio_samples_per_block; /* MSADPCM only — from cbSize extra */
    uint8_t                 *audio_extradata;   /* WAVEFORMATEX cbSize tail (MSADPCM coef table etc.) */
    int                      audio_extradata_size;
    int16_t                 *audio_buf;         /* PCM path: S16-interleaved accumulator */
    int                      audio_buf_used;
    int                      audio_buf_cap;
    int64_t                  audio_pts;         /* total samples-per-channel emitted */

    BOOL                     writer_open;
    BOOL                     writer_finalized;
    CRITICAL_SECTION         open_lock;     /* serializes writer_open_encoder against the MC vs source-thread race */
    BOOL                     open_lock_init;
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

/* MSADPCM audio subtype — fourcc-as-Data1 pattern, format tag 0x0002.
   The game's "Audio Enc" filter (a system audio compressor bound from
   CLSID_AudioCompressorCategory) defaults to producing this. We don't
   transcode it; we just mux it straight into the ASF stream. */
static const GUID MEDIASUBTYPE_MSADPCM_local =
    {0x00000002, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

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

/* Flush an encoder's queued packets and mux them. Used during normal
   close; also called per-Receive when packets come out. */
static void drain_encoder(MoviesAsfWriter *w, AVCodecContext *cc, AVStream *st, AVPacket *pk) {
    for (;;) {
        int ret = avcodec_receive_packet(cc, pk);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) break;
        pk->stream_index = st->index;
        av_packet_rescale_ts(pk, cc->time_base, st->time_base);
        av_interleaved_write_frame(w->fmt_ctx, pk);
        av_packet_unref(pk);
    }
}

/* ============================================================
 * Pre-pass WAV reader → AAC
 *
 * Audio is read directly from the TMP wav that the game's Wave Dest
 * filter wrote during the pre-pass (the in-graph audio chain is bypassed
 * entirely). See header comment for full rationale.
 * ============================================================ */

typedef struct {
    HANDLE  file;
    int     channels;
    int     sample_rate;
    int     bits_per_sample;
    int64_t data_offset;
    int64_t data_bytes;
} WavInfo;

/* Locate the TMP wav path via the CAviSyst global at 0x010bab18 (direct
   address — version-specific). The CAviSyst object stores the WCHAR
   path at +0x210 from its base. Returns NULL if anything is unsafe to
   read. */
static const WCHAR *locate_prepass_wav_path(void) {
    void **paviSyst_slot = (void **)0x010bab18;
    if (IsBadReadPtr(paviSyst_slot, sizeof(void*))) return NULL;
    void *obj = *paviSyst_slot;
    if (!obj) return NULL;
    const WCHAR *p = (const WCHAR *)((BYTE*)obj + 0x210);
    if (IsBadReadPtr(p, 4)) return NULL;
    if (*p == 0) return NULL;
    return p;
}

/* Open the TMP wav and parse just enough header to know channels,
   sample rate, bits/sample, and where the PCM body starts. The header's
   data_chunk_size is broken (placeholder ~1 GB, never seeked-and-updated
   on close); we trust the actual file size instead. Caller closes the
   returned handle. */
static BOOL wav_open_info(const WCHAR *path, WavInfo *out) {
    memset(out, 0, sizeof(*out));
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return FALSE; }
    int64_t file_size = li.QuadPart;
    if (file_size < 44) { CloseHandle(h); return FALSE; }

    BYTE riff[12]; DWORD got = 0;
    if (!ReadFile(h, riff, 12, &got, NULL) || got != 12
        || memcmp(riff, "RIFF", 4) != 0
        || memcmp(riff + 8, "WAVE", 4) != 0)
    {
        CloseHandle(h); return FALSE;
    }

    BOOL got_fmt = FALSE, got_data = FALSE;
    int64_t pos = 12;
    while (pos + 8 <= file_size) {
        BYTE ch[8];
        LARGE_INTEGER mv; mv.QuadPart = pos;
        if (!SetFilePointerEx(h, mv, NULL, FILE_BEGIN)) break;
        if (!ReadFile(h, ch, 8, &got, NULL) || got != 8) break;
        DWORD chunk_size = *(DWORD*)(ch + 4);
        int64_t next = pos + 8 + chunk_size + (chunk_size & 1);

        if (memcmp(ch, "fmt ", 4) == 0) {
            BYTE fmt[16] = {0};
            DWORD fr = 0;
            if (!ReadFile(h, fmt, 16, &fr, NULL) || fr != 16) break;
            uint16_t format_tag  = *(uint16_t*)(fmt + 0);
            uint16_t channels    = *(uint16_t*)(fmt + 2);
            uint32_t sample_rate = *(uint32_t*)(fmt + 4);
            uint16_t bits        = *(uint16_t*)(fmt + 14);
            if (format_tag != 1) {
                proxy_log("MwWriter: TMP wav is not PCM (tag=0x%04X)", format_tag);
                CloseHandle(h); return FALSE;
            }
            if (channels < 1 || channels > 2 || sample_rate < 8000 || bits != 16) {
                proxy_log("MwWriter: TMP wav unsupported format (ch=%u sr=%u bits=%u)",
                          channels, sample_rate, bits);
                CloseHandle(h); return FALSE;
            }
            out->channels        = channels;
            out->sample_rate     = sample_rate;
            out->bits_per_sample = bits;
            got_fmt = TRUE;
        } else if (memcmp(ch, "data", 4) == 0) {
            out->data_offset = pos + 8;
            out->data_bytes  = file_size - out->data_offset;
            got_data = TRUE;
            break;
        }
        if (next <= pos) break;
        pos = next;
    }

    if (!got_fmt || !got_data) {
        proxy_log("MwWriter: TMP wav missing fmt or data chunk");
        CloseHandle(h); return FALSE;
    }
    proxy_log("MwWriter: TMP wav %dHz x%d %d-bit, %lld PCM bytes (~%.2fs)",
              out->sample_rate, out->channels, out->bits_per_sample,
              (long long)out->data_bytes,
              (double)out->data_bytes / (out->sample_rate * out->channels * 2));
    out->file = h;
    return TRUE;
}

/* Read PCM body in frame_size chunks, transcode S16 interleaved → FLTP,
   encode AAC, mux. Caller must have set up acodec/astream/frame_pcm and
   called avformat_write_header before calling this. */
static HRESULT wav_encode_to_aac(MoviesAsfWriter *w, const WavInfo *wi) {
    const int channels    = wi->channels;
    const int sr          = wi->sample_rate;
    const int frame_samps = w->acodec->frame_size;
    const int bytes_per_frame = frame_samps * channels * (int)sizeof(int16_t);

    int16_t *buf = (int16_t*)malloc(bytes_per_frame);
    if (!buf) return E_OUTOFMEMORY;

    LARGE_INTEGER mv; mv.QuadPart = wi->data_offset;
    if (!SetFilePointerEx(wi->file, mv, NULL, FILE_BEGIN)) {
        free(buf); return E_FAIL;
    }

    int64_t bytes_remaining        = wi->data_bytes;
    int64_t total_samples_per_chan = 0;

    while (bytes_remaining > 0) {
        int want = (bytes_remaining > bytes_per_frame)
                   ? bytes_per_frame : (int)bytes_remaining;
        DWORD got = 0;
        if (!ReadFile(wi->file, buf, want, &got, NULL) || got == 0) break;
        int got_samples = (int)got / (channels * (int)sizeof(int16_t));
        if (got_samples == 0) break;

        int rc = av_frame_make_writable(w->frame_pcm);
        if (rc < 0) { free(buf); return E_FAIL; }
        for (int c = 0; c < channels; c++) {
            float *dst = (float*)w->frame_pcm->data[c];
            for (int s = 0; s < got_samples; s++)
                dst[s] = buf[s * channels + c] * (1.0f / 32768.0f);
            /* Zero-pad the tail on the last short read. */
            for (int s = got_samples; s < frame_samps; s++)
                dst[s] = 0.0f;
        }
        w->frame_pcm->pts        = total_samples_per_chan;
        w->frame_pcm->nb_samples = frame_samps;
        total_samples_per_chan  += got_samples;

        rc = avcodec_send_frame(w->acodec, w->frame_pcm);
        if (rc < 0) { proxy_log("  aac send_frame: %d", rc); break; }
        drain_encoder(w, w->acodec, w->astream, w->apkt);

        bytes_remaining -= got;
    }

    avcodec_send_frame(w->acodec, NULL);
    drain_encoder(w, w->acodec, w->astream, w->apkt);

    free(buf);
    proxy_log("  AAC: encoded %lld samples per channel (%.2fs)",
              (long long)total_samples_per_chan,
              (double)total_samples_per_chan / sr);
    return S_OK;
}

static void writer_close_encoder(MoviesAsfWriter *w) {
    if (w->writer_finalized || !w->writer_open) goto cleanup;

    /* Send NULL to each encoder to mark EOF, drain remaining packets. */
    if (w->vcodec) {
        avcodec_send_frame(w->vcodec, NULL);
        drain_encoder(w, w->vcodec, w->vstream, w->pkt);
    }
    if (w->acodec) {
        avcodec_send_frame(w->acodec, NULL);
        drain_encoder(w, w->acodec, w->astream, w->apkt);
    }
    if (w->fmt_ctx && (w->fmt_ctx->oformat->flags & AVFMT_NOFILE) == 0)
        av_write_trailer(w->fmt_ctx);
    w->writer_finalized = TRUE;

cleanup:
    if (w->frame_yuv) { av_frame_free(&w->frame_yuv); }
    if (w->frame_pcm) { av_frame_free(&w->frame_pcm); }
    if (w->pkt)       { av_packet_free(&w->pkt); }
    if (w->apkt)      { av_packet_free(&w->apkt); }
    if (w->vcodec)    { avcodec_free_context(&w->vcodec); }
    if (w->acodec)    { avcodec_free_context(&w->acodec); }
    if (w->audio_buf) { free(w->audio_buf); w->audio_buf = NULL; w->audio_buf_used = 0; w->audio_buf_cap = 0; }
    if (w->audio_extradata) { free(w->audio_extradata); w->audio_extradata = NULL; w->audio_extradata_size = 0; }
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
   May be called from BF_Pause/BF_Run on the MC thread AND from
   writer_push_*'s lazy fallback on a source filter's deliver thread —
   the latter races because source filters start producing samples as
   soon as their own Pause runs, and our Pause runs later in the
   cascade. Serialized via open_lock with double-checked writer_open. */
static HRESULT writer_open_encoder_inner(MoviesAsfWriter *w);

static HRESULT writer_open_encoder(MoviesAsfWriter *w) {
    if (w->writer_open) return S_OK;
    if (!w->open_lock_init) {
        /* Should never happen — mw_writer_create initialises it — but
           if it ever did, fall through and accept the race rather than
           dereferencing an uninitialised CS. */
        return writer_open_encoder_inner(w);
    }
    EnterCriticalSection(&w->open_lock);
    HRESULT hr = w->writer_open ? S_OK : writer_open_encoder_inner(w);
    LeaveCriticalSection(&w->open_lock);
    return hr;
}

static HRESULT writer_open_encoder_inner(MoviesAsfWriter *w) {
    if (!w->filename || !w->video_pin || !w->video_pin->connected) {
        proxy_log("MwWriter: cannot open — filename=%p connected=%d",
                  w->filename, w->video_pin ? w->video_pin->connected : 0);
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

    /* MP4 (mov muxer). Pass "mp4" explicitly so output format is
       unambiguous regardless of the filename's extension. */
    int rc = avformat_alloc_output_context2(&w->fmt_ctx, NULL, "mp4", u8);
    if (rc < 0 || !w->fmt_ctx) {
        proxy_log("  avformat_alloc_output_context2 failed: %d", rc);
        free(u8);
        return E_FAIL;
    }

    /* libopenh264 (BSD-2-Clause, Cisco patent royalty coverage) keeps the
       project LGPL-clean. Fallback to mpeg4 (LGPL Part 2) keeps the mod
       functional if FFmpeg was built without OpenH264 support — the file
       will play on most modern players, just less universally than H.264. */
    const AVCodec *codec = avcodec_find_encoder_by_name("libopenh264");
    if (!codec) {
        proxy_log("  libopenh264 not available, falling back to mpeg4");
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (!codec) {
        proxy_log("  no h264/mpeg4 encoder available — rebuild FFmpeg with --enable-libopenh264");
        free(u8);
        return E_FAIL;
    }
    proxy_log("  video encoder: %s", codec->name);

    w->vstream = avformat_new_stream(w->fmt_ctx, NULL);
    if (!w->vstream) { free(u8); return E_FAIL; }

    w->vcodec = avcodec_alloc_context3(codec);
    if (!w->vcodec) { free(u8); return E_FAIL; }

    w->vcodec->codec_id   = codec->id;
    w->vcodec->codec_type = AVMEDIA_TYPE_VIDEO;
    w->vcodec->width      = w->frame_w;
    w->vcodec->height     = w->frame_h;
    w->vcodec->pix_fmt    = AV_PIX_FMT_YUV420P;
    /* Bitrate: prefer the captured IWMProfile's video stream bitrate
       (matches the user's in-game quality choice — Upload/Small/Medium/
       High/Best maps to 114k/300k/300k/300k/8192k in the shipped .prx).
       Fall back to a resolution-proportional heuristic when no profile
       is available or its video bitrate is unreadable. */
    int64_t profile_bitrate = 0;
    if (w->profile) {
        IWMProfile *prof = (IWMProfile*)w->profile;
        DWORD nstreams = 0;
        if (prof->lpVtbl->GetStreamCount(prof, &nstreams) == S_OK) {
            for (DWORD i = 0; i < nstreams; i++) {
                IWMStreamConfig *sc = NULL;
                if (prof->lpVtbl->GetStream(prof, i, &sc) != S_OK || !sc) continue;
                GUID stype;
                if (sc->lpVtbl->GetStreamType(sc, &stype) == S_OK
                    && IsEqualGUID(&stype, &WMMEDIATYPE_Video))
                {
                    DWORD br = 0;
                    if (sc->lpVtbl->GetBitrate(sc, &br) == S_OK && br > 0)
                        profile_bitrate = br;
                }
                sc->lpVtbl->Release(sc);
                if (profile_bitrate) break;
            }
        }
    }
    if (profile_bitrate > 0) {
        /* Clamp: shipped profiles span 114k–8192k. Allow some headroom
           in case a future profile pushes higher; floor protects against
           a profile that would produce unwatchable output. */
        if (profile_bitrate < 50000)    profile_bitrate = 50000;
        if (profile_bitrate > 50000000) profile_bitrate = 50000000;
        w->vcodec->bit_rate = profile_bitrate;
        proxy_log("  bit_rate: %lld (from IWMProfile)", (long long)profile_bitrate);
    } else {
        w->vcodec->bit_rate = (int64_t)w->frame_w * w->frame_h * 3;
        proxy_log("  bit_rate: %lld (resolution heuristic — no profile)",
                  (long long)w->vcodec->bit_rate);
    }
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
        proxy_log("  avcodec_open2(video) failed: %d", rc);
        free(u8);
        return E_FAIL;
    }

    rc = avcodec_parameters_from_context(w->vstream->codecpar, w->vcodec);
    if (rc < 0) { free(u8); return E_FAIL; }
    w->vstream->time_base = w->vcodec->time_base;

    /* ===== Audio (AAC from pre-pass TMP wav) =====
       Three things must be true to add an audio stream:
         (a) the audio pin actually got connected by the game's graph
             builder — _NA profiles skip the audio chain entirely and
             leave the pin disconnected;
         (b) the captured IWMProfile declares an audio stream — defensive
             check in case pin wiring and profile ever disagree;
         (c) we can locate and open the pre-pass TMP wav via the
             CAviSyst global. Failing this we still produce a video-only
             file rather than aborting export.

       We bypass the in-graph audio path entirely because the game's
       Wave Dest filter writes the TMP wav with a broken data_chunk_size
       (placeholder ~1 GB) that confuses every downstream parser we tried.
       The PCM body itself is correct — we just trust file size, not the
       header field. */
    BOOL profile_has_audio = FALSE;
    int64_t aprofile_bitrate = 0;
    if (w->profile) {
        IWMProfile *prof = (IWMProfile*)w->profile;
        DWORD nstreams = 0;
        if (prof->lpVtbl->GetStreamCount(prof, &nstreams) == S_OK) {
            for (DWORD i = 0; i < nstreams; i++) {
                IWMStreamConfig *sc = NULL;
                if (prof->lpVtbl->GetStream(prof, i, &sc) != S_OK || !sc) continue;
                GUID stype;
                if (sc->lpVtbl->GetStreamType(sc, &stype) == S_OK
                    && IsEqualGUID(&stype, &WMMEDIATYPE_Audio))
                {
                    profile_has_audio = TRUE;
                    DWORD br = 0;
                    if (sc->lpVtbl->GetBitrate(sc, &br) == S_OK && br > 0)
                        aprofile_bitrate = br;
                }
                sc->lpVtbl->Release(sc);
                if (profile_has_audio && aprofile_bitrate) break;
            }
        }
    }
    proxy_log("  audio gate: pin_connected=%d profile_has_audio=%d",
              w->audio_pin && w->audio_pin->connected, profile_has_audio);

    /* Open the TMP wav and parse its header so we know channels/sample
       rate before avformat_write_header. The actual PCM-to-AAC encode
       happens after write_header (see wav_encode_to_aac call below). */
    WavInfo wi = {0};
    BOOL audio_ok = FALSE;
    if (w->audio_pin && w->audio_pin->connected && profile_has_audio) {
        const WCHAR *tmp_path = locate_prepass_wav_path();
        if (tmp_path) {
            audio_ok = wav_open_info(tmp_path, &wi);
            if (!audio_ok) {
                proxy_log("  audio: TMP wav unreadable at %ls — video-only export", tmp_path);
            }
        } else {
            proxy_log("  audio: pre-pass wav path not locatable — video-only export");
        }
    }
    if (audio_ok) {
        const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!acodec) {
            proxy_log("  aac encoder not available — video-only export");
            CloseHandle(wi.file);
            audio_ok = FALSE;
        }
        if (audio_ok) {
            w->astream = avformat_new_stream(w->fmt_ctx, NULL);
            if (!w->astream) { CloseHandle(wi.file); free(u8); return E_FAIL; }
            w->apkt   = av_packet_alloc();
            w->acodec = avcodec_alloc_context3(acodec);
            if (!w->acodec) { CloseHandle(wi.file); free(u8); return E_FAIL; }
            w->acodec->codec_id     = acodec->id;
            w->acodec->codec_type   = AVMEDIA_TYPE_AUDIO;
            w->acodec->sample_rate  = wi.sample_rate;
            w->acodec->ch_layout.order = AV_CHANNEL_ORDER_NATIVE;
            w->acodec->ch_layout.nb_channels = wi.channels;
            w->acodec->ch_layout.u.mask = (wi.channels == 2)
                                          ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
            w->acodec->sample_fmt   = AV_SAMPLE_FMT_FLTP;
            if (aprofile_bitrate > 0) {
                if (aprofile_bitrate < 32000)  aprofile_bitrate = 32000;
                if (aprofile_bitrate > 320000) aprofile_bitrate = 320000;
                w->acodec->bit_rate = aprofile_bitrate;
            } else {
                w->acodec->bit_rate = 128000LL;
            }
            w->acodec->time_base = (AVRational){ 1, wi.sample_rate };
            if (w->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                w->acodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            rc = avcodec_open2(w->acodec, acodec, NULL);
            if (rc < 0) {
                proxy_log("  avcodec_open2(aac) failed: %d — video-only export", rc);
                avcodec_free_context(&w->acodec);
                w->astream = NULL;
                CloseHandle(wi.file);
                audio_ok = FALSE;
            } else {
                avcodec_parameters_from_context(w->astream->codecpar, w->acodec);
                w->astream->time_base = w->acodec->time_base;

                w->frame_pcm = av_frame_alloc();
                w->frame_pcm->format      = AV_SAMPLE_FMT_FLTP;
                w->frame_pcm->nb_samples  = w->acodec->frame_size;
                w->frame_pcm->sample_rate = wi.sample_rate;
                w->frame_pcm->ch_layout   = w->acodec->ch_layout;
                av_frame_get_buffer(w->frame_pcm, 0);
                proxy_log("  audio encoder: aac %dHz x%d frame_size=%d bit_rate=%lld",
                          wi.sample_rate, wi.channels,
                          w->acodec->frame_size, (long long)w->acodec->bit_rate);
            }
        }
    }

    rc = avio_open(&w->fmt_ctx->pb, u8, AVIO_FLAG_WRITE);
    free(u8);
    if (rc < 0) {
        proxy_log("  avio_open failed: %d", rc);
        return E_FAIL;
    }

    rc = avformat_write_header(w->fmt_ctx, NULL);
    if (rc < 0) {
        proxy_log("  avformat_write_header failed: %d", rc);
        if (audio_ok) CloseHandle(wi.file);
        return E_FAIL;
    }

    /* Encode the entire pre-pass WAV body now, before any video frames
       arrive. AAC packets get interleaved into the file via
       av_interleaved_write_frame; the muxer's interleaver will hold them
       until video PTSs catch up. We do it all up-front because the WAV
       contains the full audio track anyway — no reason to drip-feed. */
    if (audio_ok) {
        wav_encode_to_aac(w, &wi);
        CloseHandle(wi.file);
    }

    /* YUV scratch frame for video. align=0 lets libav pick natural alignment. */
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
    proxy_log("MwWriter: encoder open (%dx%d, src_pix=%d, %d/%d fps, video=%s%s)",
              w->frame_w, w->frame_h, w->input_pix_fmt,
              w->vframe_rate.num, w->vframe_rate.den, codec->name,
              w->astream ? " + aac" : "");
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

    /* Pace from the sample's REFERENCE_TIME, not a frame counter.
       Movies' export render thread can't keep up with 30 fps real-time
       (we observed ~22 actual fps over a 30 s movie), but every sample
       carries an accurate in-game timeline pts in 100ns units. Frame
       counting compresses the 30 s movie into ~22 s of output that runs
       too fast against the 30 s audio track; rescaling pts_100ns into
       the codec time_base preserves the original timing — H.264 in MP4
       accepts variable per-frame durations natively.

       Monotonic clamp: time_base is 1/30 and the game can occasionally
       deliver two frames inside the same 1/30s bucket (jitter near the
       rounding boundary). The encoder rejects equal/decreasing pts, so
       force a +1 step in that case. */
    int64_t pts;
    if (pts_100ns > 0 || w->next_pts == 0) {
        pts = av_rescale_q(pts_100ns,
                           (AVRational){1, 10000000},
                           w->vcodec->time_base);
        if (pts < w->next_pts) pts = w->next_pts;
    } else {
        pts = w->next_pts;
    }
    w->next_pts = pts + 1;
    w->frame_yuv->pts = pts;

    int rc = avcodec_send_frame(w->vcodec, w->frame_yuv);
    if (rc < 0) {
        proxy_log("  send_frame: %d", rc);
        return E_FAIL;
    }
    drain_encoder(w, w->vcodec, w->vstream, w->pkt);
    return S_OK;
}

/* Audio samples arriving via the input pin are discarded — audio is read
   directly from the pre-pass TMP wav at writer_open_encoder time. The pin
   still needs to accept the connection (the game's graph builder requires
   it), but nothing it pushes us is useful. */

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
        if (w->video_pin) {
            mt_clear(&w->video_pin->mt, &w->video_pin->mt_format_buf);
            free(w->video_pin);
        }
        if (w->audio_pin) {
            mt_clear(&w->audio_pin->mt, &w->audio_pin->mt_format_buf);
            free(w->audio_pin);
        }
        if (w->open_lock_init) {
            DeleteCriticalSection(&w->open_lock);
            w->open_lock_init = FALSE;
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
    /* Open both encoders here — graph is fully built by Pause time, and
       Pause is single-threaded from the graph control. Avoids the race
       where video/audio Receive threads both try to lazy-open. */
    if (!w->writer_open && w->video_pin && w->video_pin->connected)
        writer_open_encoder(w);
    w->state = State_Paused;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE BF_Run(IBaseFilter_DS *This, REFERENCE_TIME tStart) {
    MoviesAsfWriter *w = (MoviesAsfWriter*)This;
    proxy_log("MwWriter::Run(t=%lld)", (long long)tStart);
    if (!w->writer_open && w->video_pin && w->video_pin->connected)
        writer_open_encoder(w);
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
    if (Id && w->video_pin && wcscmp(Id, w->video_pin->pin_id) == 0) {
        *ppPin = (IPin_DS*)w->video_pin;
        InterlockedIncrement(&w->video_pin->ref_count);
        return S_OK;
    }
    if (Id && w->audio_pin && wcscmp(Id, w->audio_pin->pin_id) == 0) {
        *ppPin = (IPin_DS*)w->audio_pin;
        InterlockedIncrement(&w->audio_pin->ref_count);
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

/* IEnumPins — walks video then audio pin (audio omitted if not present). */
typedef struct {
    IEnumPinsVtbl_DS *lpVtbl;
    LONG              ref;
    MoviesAsfWriter  *parent;
    int               cursor;
} MwEnumPins;
static IEnumPinsVtbl_DS g_MwEnumPinsVtbl;
static const GUID IID_IEnumPins_local =
    {0x56A86892, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

static MwInputPin *enum_pin_at(MoviesAsfWriter *w, int idx) {
    if (idx == 0) return w->video_pin;
    if (idx == 1) return w->audio_pin;
    return NULL;
}
static int enum_pin_count(MoviesAsfWriter *w) {
    int n = 0;
    if (w->video_pin) n++;
    if (w->audio_pin) n++;
    return n;
}

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
    int total = enum_pin_count(e->parent);
    ULONG n = 0;
    while (n < cReq && e->cursor < total) {
        MwInputPin *p = enum_pin_at(e->parent, e->cursor);
        if (!p) break;
        out[n] = (IPin_DS*)p;
        InterlockedIncrement(&p->ref_count);
        n++;
        e->cursor++;
    }
    if (fetched) *fetched = n;
    return (n == cReq) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EP_Skip(IEnumPins_DS *This, ULONG c) {
    MwEnumPins *e = (MwEnumPins*)This;
    int total = enum_pin_count(e->parent);
    e->cursor += c; if (e->cursor > total) e->cursor = total;
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
        /* The game always asks for .wmv; we actually produce MP4. Rewrite
           the trailing extension so the file is correctly named on disk
           (case-insensitive match — Lionhead's UI uses lowercase but
           defensive against future patches). */
        if (n >= 4) {
            WCHAR *ext = w->filename + n - 4;
            if (ext[0] == L'.'
                && (ext[1] == L'w' || ext[1] == L'W')
                && (ext[2] == L'm' || ext[2] == L'M')
                && (ext[3] == L'v' || ext[3] == L'V'))
            {
                ext[1] = L'm';
                ext[2] = L'p';
                ext[3] = L'4';
                proxy_log("MwWriter::SetFileName → rewritten to %ls", w->filename);
            }
        }
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
static HRESULT pin_accept_video(MwInputPin *p, const AM_MEDIA_TYPE *pmt) {
    int avpix = dshow_subtype_to_avpix(&pmt->subtype);
    if (avpix == AV_PIX_FMT_NONE) {
        proxy_log("  video: unknown subtype — refusing");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    int w = 0, h = 0, stride = 0;
    BOOL flip_v = FALSE;
    int bicompression = 0, bibitcount = 0;
    AVRational fr = {30, 1};
    if (pmt->cbFormat >= sizeof(VIDEOINFOHEADER) && pmt->pbFormat) {
        const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER*)pmt->pbFormat;
        w = vih->bmiHeader.biWidth;
        /* DIB convention: positive biHeight = bottom-up, negative = top-down. */
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
        proxy_log("  video: invalid dimensions w=%d h=%d", w, h);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    p->parent->frame_w       = w;
    p->parent->frame_h       = h;
    p->parent->input_pix_fmt = avpix;
    p->parent->input_stride  = stride;
    p->parent->input_flip_v  = flip_v;
    p->parent->vframe_rate   = fr;
    p->parent->next_pts      = 0;
    proxy_log("  video accepted: %dx%d stride=%d avpix=%d fps=%d/%d flip_v=%d biCompression=0x%08X biBitCount=%d",
              w, h, stride, avpix, fr.num, fr.den, flip_v, bicompression, bibitcount);
    return S_OK;
}

static HRESULT pin_accept_audio(MwInputPin *p, const AM_MEDIA_TYPE *pmt) {
    /* Two subtypes accepted:
        PCM     → we transcode to WMA2 with libavcodec wmav2.
        MSADPCM → we mux passthrough; ASF carries it natively.
       The game's "Audio Enc" filter (a system codec bound from
       CLSID_AudioCompressorCategory) defaults to MSADPCM and isn't
       reconfigurable from our side without reimplementing more of the
       graph. Accepting MSADPCM is the path of least resistance and
       produces a playable file (modern players handle it).  */
    BOOL is_pcm     = IsEqualGUID(&pmt->subtype, &WMMEDIASUBTYPE_PCM);
    BOOL is_msadpcm = IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_MSADPCM_local);
    if (!is_pcm && !is_msadpcm) {
        proxy_log("  audio: subtype not PCM/MSADPCM — refusing (sub.Data1=0x%08lX)",
                  pmt->subtype.Data1);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (pmt->cbFormat < sizeof(WAVEFORMATEX) || !pmt->pbFormat) {
        proxy_log("  audio: missing/short WAVEFORMATEX (cbFormat=%lu)", (unsigned long)pmt->cbFormat);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    const WAVEFORMATEX *wfx = (const WAVEFORMATEX*)pmt->pbFormat;
    if (is_pcm && wfx->wFormatTag != WAVE_FORMAT_PCM) {
        proxy_log("  audio: PCM subtype but wFormatTag=0x%04X — refusing", wfx->wFormatTag);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (is_msadpcm && wfx->wFormatTag != 0x0002) {
        proxy_log("  audio: MSADPCM subtype but wFormatTag=0x%04X — refusing", wfx->wFormatTag);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (wfx->nChannels != 1 && wfx->nChannels != 2) {
        proxy_log("  audio: nChannels=%u not 1/2 — refusing", wfx->nChannels);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    MoviesAsfWriter *w = p->parent;
    w->audio_sample_rate     = (int)wfx->nSamplesPerSec;
    w->audio_channels        = (int)wfx->nChannels;
    w->audio_bits_per_sample = (int)wfx->wBitsPerSample;
    w->audio_block_align     = (int)wfx->nBlockAlign;
    w->audio_pts             = 0;
    w->audio_codec_id        = is_pcm ? AV_CODEC_ID_PCM_S16LE : AV_CODEC_ID_ADPCM_MS;

    /* Capture the WAVEFORMATEX cbSize tail. For MSADPCM this carries
       wSamplesPerBlock + wNumCoef + ADPCMCOEFSET[wNumCoef], which the
       muxer needs as extradata so the codec is described correctly in
       the ASF header. */
    free(w->audio_extradata);
    w->audio_extradata = NULL;
    w->audio_extradata_size = 0;
    w->audio_samples_per_block = 0;
    if (wfx->cbSize > 0 && pmt->cbFormat >= sizeof(WAVEFORMATEX) + wfx->cbSize) {
        w->audio_extradata = (uint8_t*)malloc(wfx->cbSize);
        if (w->audio_extradata) {
            memcpy(w->audio_extradata, (const uint8_t*)wfx + sizeof(WAVEFORMATEX), wfx->cbSize);
            w->audio_extradata_size = wfx->cbSize;
        }
        if (is_msadpcm && wfx->cbSize >= 2) {
            w->audio_samples_per_block = *(const uint16_t*)((const uint8_t*)wfx + sizeof(WAVEFORMATEX));
        }
    }

    proxy_log("  audio accepted: %luHz x%u %u-bit %s blockAlign=%u sampPerBlock=%d extra=%d",
              (unsigned long)wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample,
              is_pcm ? "PCM" : "MSADPCM",
              wfx->nBlockAlign, w->audio_samples_per_block, w->audio_extradata_size);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ReceiveConnection(IPin_DS *This, IPin_DS *peer, const AM_MEDIA_TYPE *pmt) {
    MwInputPin *p = (MwInputPin*)This;
    if (!peer || !pmt) return E_POINTER;
    if (p->connected) return VFW_E_ALREADY_CONNECTED;

    proxy_log("MwInputPin[%s]::ReceiveConnection major={%08lX} sub={%08lX} cbFormat=%lu",
              p->kind == PIN_KIND_VIDEO ? "video" : "audio",
              pmt->majortype.Data1, pmt->subtype.Data1, (unsigned long)pmt->cbFormat);

    HRESULT hr;
    if (p->kind == PIN_KIND_VIDEO) {
        if (!IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Video)) return VFW_E_TYPE_NOT_ACCEPTED;
        hr = pin_accept_video(p, pmt);
    } else {
        if (!IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Audio)) return VFW_E_TYPE_NOT_ACCEPTED;
        hr = pin_accept_audio(p, pmt);
    }
    if (hr != S_OK) return hr;

    mt_clear(&p->mt, &p->mt_format_buf);
    mt_copy(&p->mt, &p->mt_format_buf, pmt);

    p->peer = peer;
    peer->lpVtbl->AddRef(peer);
    p->connected = TRUE;
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
    MwInputPin *p = (MwInputPin*)This;
    if (!pmt) return E_POINTER;
    if (p->kind == PIN_KIND_VIDEO) {
        if (!IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Video)) return S_FALSE;
        return (dshow_subtype_to_avpix(&pmt->subtype) != AV_PIX_FMT_NONE) ? S_OK : S_FALSE;
    }
    /* Audio: accept PCM and MSADPCM. See pin_accept_audio for rationale. */
    if (!IsEqualGUID(&pmt->majortype, &WMMEDIATYPE_Audio)) return S_FALSE;
    if (IsEqualGUID(&pmt->subtype, &WMMEDIASUBTYPE_PCM))         return S_OK;
    if (IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_MSADPCM_local)) return S_OK;
    return S_FALSE;
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
    if (p->kind == PIN_KIND_AUDIO) {
        proxy_log("MwInputPin[audio]::EndOfStream — total audio_pts=%lld samples (~%.2fs)",
                  (long long)p->parent->audio_pts,
                  p->parent->audio_sample_rate
                    ? (double)p->parent->audio_pts / p->parent->audio_sample_rate
                    : 0.0);
    } else {
        proxy_log("MwInputPin[video]::EndOfStream — total frames=%lld",
                  (long long)p->parent->next_pts);
    }
    /* Don't close the encoder here. EOS arrives per-pin and may fire
       when one stream has finished while the other is still running.
       Closing on the first EOS prematurely calls av_write_trailer with
       streams that have received zero packets, which crashes inside
       libavformat (observed with MSADPCM passthrough). BF_Stop is the
       natural close point, and the FakeGraph::MC::StopWhenReady fix
       guarantees Stop is forwarded to us during graph teardown. */
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

    /* Audio: drop. Real audio comes from the pre-pass TMP wav. */
    if (p->kind == PIN_KIND_AUDIO) return S_OK;

    BYTE *buffer = NULL;
    pSample->lpVtbl->GetPointer(pSample, &buffer);
    long actual = pSample->lpVtbl->GetActualDataLength(pSample);
    REFERENCE_TIME tStart = 0, tEnd = 0;
    pSample->lpVtbl->GetTime(pSample, &tStart, &tEnd);

    if (!buffer || actual <= 0) return S_OK;

    /* Video. Stride was computed at ReceiveConnection (DWORD-aligned
       width * bpp/8). Some sources pack differently; if biBitCount
       stride disagrees with actual_data_length / height we trust the
       derived value. */
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
    InitializeCriticalSection(&w->open_lock);
    w->open_lock_init         = TRUE;
    wcscpy(w->filter_name, L"WM ASF Writer");

    MwInputPin *vpin = (MwInputPin*)calloc(1, sizeof(*vpin));
    if (!vpin) { free(w); return E_OUTOFMEMORY; }
    vpin->lpVtbl            = &g_MwInputPinVtbl;
    vpin->lpMemInputPinVtbl = &g_MwInputMemInputPinVtbl;
    vpin->ref_count         = 1;
    vpin->parent            = w;
    vpin->kind              = PIN_KIND_VIDEO;
    wcscpy(vpin->pin_id, L"VideoInput01");
    w->video_pin = vpin;

    MwInputPin *apin = (MwInputPin*)calloc(1, sizeof(*apin));
    if (!apin) { free(vpin); free(w); return E_OUTOFMEMORY; }
    apin->lpVtbl            = &g_MwInputPinVtbl;
    apin->lpMemInputPinVtbl = &g_MwInputMemInputPinVtbl;
    apin->ref_count         = 1;
    apin->parent            = w;
    apin->kind              = PIN_KIND_AUDIO;
    wcscpy(apin->pin_id, L"AudioInput02");
    w->audio_pin = apin;

    *ppFilter = (IBaseFilter_DS*)w;
    proxy_log("MwWriter created @ %p (video+audio pins, replacing CLSID_WMAsfWriter)", w);
    return S_OK;
}
