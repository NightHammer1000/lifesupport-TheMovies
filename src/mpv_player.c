#include "mpv_player.h"
#include "log.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

struct mpv_player {
    mpv_handle           *mpv;
    mpv_render_context   *render;

    HANDLE                update_event;   /* signalled by mpv update callback */
    HANDLE                stop_event;
    HANDLE                render_thread;

    int                   width;
    int                   height;
    size_t                stride_bytes;   /* width*4, aligned up to 64 */
    BYTE                 *bgr0_buf;
    double                duration_sec;   /* cached from load */

    mpv_frame_cb_t        frame_cb;
    void                 *frame_cb_user;

    CRITICAL_SECTION      cs;             /* guards size + buffer + frame_cb */
};

/* ---------- update callback (called from mpv's render thread) ---------- */
static void on_mpv_update(void *user) {
    mpv_player_t *p = (mpv_player_t*)user;
    SetEvent(p->update_event);
}

/* ---------- size helpers ---------- */
static size_t align64(size_t v) { return (v + 63u) & ~(size_t)63; }

static void ensure_buffer_locked(mpv_player_t *p, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (p->bgr0_buf && p->width == w && p->height == h) return;
    free(p->bgr0_buf);
    p->width  = w;
    p->height = h;
    p->stride_bytes = align64((size_t)w * 4u);
    p->bgr0_buf = malloc(p->stride_bytes * (size_t)h);
    proxy_log("mpv: buffer (re)allocated %dx%d stride=%u",
              w, h, (unsigned)p->stride_bytes);
}

/* ---------- mpv event drain ---------- */
static void drain_events(mpv_player_t *p) {
    for (;;) {
        mpv_event *ev = mpv_wait_event(p->mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message *m = ev->data;
                /* mpv text already ends with a newline */
                proxy_log("mpv[%s]: %.*s", m->prefix,
                          (int)strcspn(m->text, "\r\n"), m->text);
                break;
            }
            case MPV_EVENT_END_FILE:
                proxy_log("mpv: END_FILE");
                break;
            case MPV_EVENT_FILE_LOADED:
                proxy_log("mpv: FILE_LOADED");
                break;
            default: break;
        }
    }
}

/* ---------- render thread ---------- */
static unsigned __stdcall render_thread_proc(void *arg) {
    mpv_player_t *p = (mpv_player_t*)arg;
    HANDLE waits[2] = { p->update_event, p->stop_event };
    proxy_log("mpv render thread started (tid=%lu)", GetCurrentThreadId());

    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 1) break;            /* stop */
        if (r != WAIT_OBJECT_0) continue;

        drain_events(p);

        uint64_t flags = mpv_render_context_update(p->render);
        if (!(flags & MPV_RENDER_UPDATE_FRAME)) continue;

        /* Pick up current width/height from mpv (may change at first frame). */
        int64_t mw = 0, mh = 0;
        mpv_get_property(p->mpv, "width",  MPV_FORMAT_INT64, &mw);
        mpv_get_property(p->mpv, "height", MPV_FORMAT_INT64, &mh);

        EnterCriticalSection(&p->cs);
        if (mw > 0 && mh > 0) ensure_buffer_locked(p, (int)mw, (int)mh);
        int w = p->width, h = p->height;
        size_t stride = p->stride_bytes;
        BYTE *buf = p->bgr0_buf;
        mpv_frame_cb_t cb = p->frame_cb;
        void *cb_user = p->frame_cb_user;
        LeaveCriticalSection(&p->cs);

        if (!buf || w <= 0 || h <= 0) continue;

        int sw_size[2] = { w, h };
        const char *sw_format = "bgr0";
        size_t sw_stride = stride;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_SW_SIZE,    sw_size },
            { MPV_RENDER_PARAM_SW_FORMAT,  (void*)sw_format },
            { MPV_RENDER_PARAM_SW_STRIDE,  &sw_stride },
            { MPV_RENDER_PARAM_SW_POINTER, buf },
            { 0, NULL }
        };
        int rc = mpv_render_context_render(p->render, params);
        if (rc < 0) {
            proxy_log("mpv: render_context_render failed: %d (%s)",
                      rc, mpv_error_string(rc));
            continue;
        }

        /* mpv-reported PTS in seconds → 100 ns. */
        double pos_sec = 0.0;
        mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos_sec);
        LONGLONG pts = (LONGLONG)(pos_sec * 10000000.0);

        if (cb) cb(cb_user, buf, w, h, (int)stride, pts);
    }

    proxy_log("mpv render thread exiting");
    return 0;
}

/* ---------- public API ---------- */

mpv_player_t *mpv_player_create(void) {
    mpv_player_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    InitializeCriticalSection(&p->cs);
    p->update_event = CreateEventA(NULL, FALSE, FALSE, NULL); /* auto-reset */
    p->stop_event   = CreateEventA(NULL, TRUE,  FALSE, NULL); /* manual reset */

    p->mpv = mpv_create();
    if (!p->mpv) { proxy_log("mpv_create failed"); goto fail; }

    /* Pre-init options. The render API requires vo=libmpv. */
    mpv_set_option_string(p->mpv, "vo",            "libmpv");
    mpv_set_option_string(p->mpv, "ao",            "wasapi,winmm");
    mpv_set_option_string(p->mpv, "audio-display", "no");
    mpv_set_option_string(p->mpv, "osd-level",     "0");
    /* keep-open=yes: when an unlooped file ends, pause on the last frame
       (time-pos stays at duration) instead of unloading. The game's
       IsFinished check polls position vs duration to detect completion. */
    mpv_set_option_string(p->mpv, "keep-open",     "yes");
    mpv_set_option_string(p->mpv, "hwdec",         "no");
    mpv_set_option_string(p->mpv, "input-default-bindings", "no");
    mpv_set_option_string(p->mpv, "input-vo-keyboard",      "no");
    mpv_set_option_string(p->mpv, "msg-level",     "all=warn");
    mpv_set_option_string(p->mpv, "loop-file",     "no");

    int rc = mpv_initialize(p->mpv);
    if (rc < 0) { proxy_log("mpv_initialize failed: %d", rc); goto fail; }

    mpv_request_log_messages(p->mpv, "warn");

    /* Software render context. */
    char api_type[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param create_params[] = {
        { MPV_RENDER_PARAM_API_TYPE, api_type },
        { 0, NULL }
    };
    rc = mpv_render_context_create(&p->render, p->mpv, create_params);
    if (rc < 0) {
        proxy_log("mpv_render_context_create failed: %d (%s)",
                  rc, mpv_error_string(rc));
        goto fail;
    }

    mpv_render_context_set_update_callback(p->render, on_mpv_update, p);

    p->render_thread = (HANDLE)_beginthreadex(NULL, 0, render_thread_proc, p, 0, NULL);
    if (!p->render_thread) goto fail;

    proxy_log("mpv player created");
    return p;

fail:
    mpv_player_destroy(p);
    return NULL;
}

HRESULT mpv_player_load(mpv_player_t *p, const char *path_utf8, BOOL loop,
                       int *out_w, int *out_h, double *out_fps,
                       double *out_duration_sec)
{
    if (!p || !path_utf8) return E_POINTER;

    mpv_set_option_string(p->mpv, "loop-file", loop ? "inf" : "no");

    /* Start paused so the caller controls the actual play moment. */
    int yes = 1;
    mpv_set_property(p->mpv, "pause", MPV_FORMAT_FLAG, &yes);

    const char *cmd[] = { "loadfile", path_utf8, NULL };
    int rc = mpv_command(p->mpv, cmd);
    if (rc < 0) {
        proxy_log("mpv loadfile failed: %d (%s)", rc, mpv_error_string(rc));
        return E_FAIL;
    }

    /* Wait for FILE_LOADED so we know dimensions etc. */
    DWORD start = GetTickCount();
    BOOL loaded = FALSE;
    while (GetTickCount() - start < 5000) {
        mpv_event *ev = mpv_wait_event(p->mpv, 0.1);
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) { loaded = TRUE; break; }
        if (ev->event_id == MPV_EVENT_END_FILE) {
            proxy_log("mpv: END_FILE during load");
            return E_FAIL;
        }
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            proxy_log("mpv[%s]: %.*s", m->prefix,
                      (int)strcspn(m->text, "\r\n"), m->text);
        }
    }
    if (!loaded) { proxy_log("mpv: timeout waiting for FILE_LOADED"); return E_FAIL; }

    int64_t w = 0, h = 0;
    double fps = 0.0, dur = 0.0;
    mpv_get_property(p->mpv, "width",         MPV_FORMAT_INT64,  &w);
    mpv_get_property(p->mpv, "height",        MPV_FORMAT_INT64,  &h);
    mpv_get_property(p->mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps);
    mpv_get_property(p->mpv, "duration",      MPV_FORMAT_DOUBLE, &dur);
    if (fps <= 0.0)
        mpv_get_property(p->mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps);

    proxy_log("mpv: loaded %s (%lldx%lld @ %.3f fps, dur=%.3fs)",
              path_utf8, (long long)w, (long long)h, fps, dur);

    if (w <= 0 || h <= 0) return E_FAIL;

    EnterCriticalSection(&p->cs);
    ensure_buffer_locked(p, (int)w, (int)h);
    LeaveCriticalSection(&p->cs);

    p->duration_sec = (dur > 0.0) ? dur : 0.0;

    if (out_w) *out_w = (int)w;
    if (out_h) *out_h = (int)h;
    if (out_fps) *out_fps = (fps > 0.0) ? fps : 30.0;
    if (out_duration_sec) *out_duration_sec = p->duration_sec;

    return S_OK;
}

void mpv_player_set_frame_callback(mpv_player_t *p, mpv_frame_cb_t cb, void *user) {
    if (!p) return;
    EnterCriticalSection(&p->cs);
    p->frame_cb = cb;
    p->frame_cb_user = user;
    LeaveCriticalSection(&p->cs);
}

double mpv_player_get_position(mpv_player_t *p) {
    if (!p || !p->mpv) return 0.0;
    /* When the file ends and keep-open holds the last frame, time-pos may
       lag duration by a frame or two. Clamp to duration once mpv signals
       eof-reached so the game's "is finished" check fires. */
    int eof = 0;
    mpv_get_property(p->mpv, "eof-reached", MPV_FORMAT_FLAG, &eof);
    if (eof && p->duration_sec > 0.0) return p->duration_sec;
    double pos = 0.0;
    mpv_get_property(p->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

void mpv_player_set_pause(mpv_player_t *p, BOOL paused) {
    if (!p || !p->mpv) return;
    int v = paused ? 1 : 0;
    mpv_set_property(p->mpv, "pause", MPV_FORMAT_FLAG, &v);
}

void mpv_player_set_volume_centibels(mpv_player_t *p, long cb) {
    if (!p || !p->mpv) return;
    /* IBasicAudio: 0 = full, -10000 = silent. mpv volume: 0..100 (linear %). */
    double vol;
    if (cb >= 0)            vol = 100.0;
    else if (cb <= -10000)  vol = 0.0;
    else                    vol = pow(10.0, (double)cb / 2000.0) * 100.0;
    mpv_set_property(p->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void mpv_player_destroy(mpv_player_t *p) {
    if (!p) return;

    if (p->stop_event)   SetEvent(p->stop_event);
    if (p->update_event) SetEvent(p->update_event); /* wake render thread */

    if (p->render_thread) {
        WaitForSingleObject(p->render_thread, 5000);
        CloseHandle(p->render_thread);
    }
    if (p->render) {
        mpv_render_context_free(p->render);
        p->render = NULL;
    }
    if (p->mpv) {
        mpv_terminate_destroy(p->mpv);
        p->mpv = NULL;
    }
    if (p->update_event) CloseHandle(p->update_event);
    if (p->stop_event)   CloseHandle(p->stop_event);
    DeleteCriticalSection(&p->cs);
    free(p->bgr0_buf);
    free(p);
}
