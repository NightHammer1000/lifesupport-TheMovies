#ifndef MEDIA_PROPS_H
#define MEDIA_PROPS_H

#include "wm_types.h"

typedef struct ProxyOutputMediaProps {
    IWMOutputMediaPropsVtbl *lpVtbl;
    LONG ref_count;
    WM_MEDIA_TYPE media_type;
    BYTE *format_buf;
} ProxyOutputMediaProps;

ProxyOutputMediaProps *output_media_props_create(const WM_MEDIA_TYPE *type);

#endif
