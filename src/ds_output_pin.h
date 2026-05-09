#ifndef DS_OUTPUT_PIN_H
#define DS_OUTPUT_PIN_H

#include "ds_types.h"

typedef struct DSSourceFilter DSSourceFilter;

/* Output pin handed to TEXTURERENDERER. Always video, always BGR24. libmpv
   owns audio output (WASAPI), so this only delivers decoded video frames. */
typedef struct DSOutputPin {
    IPinVtbl_DS           *lpVtbl;
    LONG                   ref_count;

    DSSourceFilter        *filter;
    WCHAR                  pin_id[16];

    /* Connected peer */
    IPin_DS               *peer;
    IMemInputPin_DS       *peer_mem;
    IMemAllocator_DS      *allocator;
    AM_MEDIA_TYPE          mt;
    BYTE                  *mt_format_buf;
    BOOL                   connected;
    BOOL                   flushing;

    int                    width, height;

    LONGLONG               current_time;
} DSOutputPin;

DSOutputPin *ds_output_pin_create(DSSourceFilter *filter, int width, int height);
void         ds_output_pin_get_media_type(DSOutputPin *pin, AM_MEDIA_TYPE *mt);
HRESULT      ds_output_pin_deliver(DSOutputPin *pin, BYTE *data, long size,
                                   REFERENCE_TIME start, REFERENCE_TIME end);

#endif
