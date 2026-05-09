#ifndef NSS_BUFFER_H
#define NSS_BUFFER_H

#include "wm_types.h"

typedef struct ProxyNSSBuffer {
    INSSBufferVtbl *lpVtbl;
    LONG ref_count;
    BYTE *data;
    DWORD length;
    DWORD max_length;
} ProxyNSSBuffer;

ProxyNSSBuffer *nss_buffer_create(DWORD max_size);

#endif
