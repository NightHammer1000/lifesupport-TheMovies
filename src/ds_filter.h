#ifndef DS_FILTER_H
#define DS_FILTER_H

#include "ds_types.h"

HRESULT ds_source_filter_create(IBaseFilter_DS **ppFilter);

IBasicAudio_DS *ds_source_filter_get_basic_audio(IBaseFilter_DS *pFilter);
void *ds_source_filter_get_media_position(IBaseFilter_DS *pFilter);
BOOL ds_source_filter_wait_first_frame(IBaseFilter_DS *pFilter, DWORD timeout_ms);

#endif
