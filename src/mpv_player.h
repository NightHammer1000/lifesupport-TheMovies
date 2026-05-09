#ifndef MPV_PLAYER_H
#define MPV_PLAYER_H

#include <windows.h>

typedef struct mpv_player mpv_player_t;

/* Frame callback. Called from the render thread for each produced video frame.
   Buffer is BGR0 (4 bytes/pixel, top-down). pts_100ns is in 100-ns units
   (DirectShow REFERENCE_TIME, but typed as LONGLONG to keep this header free
   of strmif.h). Return value is currently ignored. */
typedef HRESULT (*mpv_frame_cb_t)(void *user, const BYTE *bgr0, int width,
                                  int height, int stride_bytes,
                                  LONGLONG pts_100ns);

mpv_player_t *mpv_player_create(void);

/* Load a file. Blocks (up to ~5 s) until video size is known. On success,
   width/height/fps/duration are reported via the out pointers. */
HRESULT mpv_player_load(mpv_player_t *p, const char *path_utf8, BOOL loop,
                       int *out_w, int *out_h, double *out_fps,
                       double *out_duration_sec);

/* Set the per-frame callback. The render thread calls it with BGR0 data. */
void mpv_player_set_frame_callback(mpv_player_t *p, mpv_frame_cb_t cb, void *user);

double mpv_player_get_position(mpv_player_t *p);   /* seconds */
void   mpv_player_set_pause(mpv_player_t *p, BOOL paused);

/* Volume in centibels (DirectShow IBasicAudio scale: 0 = full, -10000 = mute). */
void   mpv_player_set_volume_centibels(mpv_player_t *p, long cb);

void   mpv_player_destroy(mpv_player_t *p);

#endif
