#ifndef ASF_WRITER_H
#define ASF_WRITER_H

#include "ds_types.h"

/* Drop-in replacement for qasf.dll's WMAsfWriter filter.
   The game CoCreates CLSID_WMAsfWriter; our hook routes the call here
   instead, returning a filter backed by libavformat (asf_stream muxer)
   + libavcodec (msmpeg4v2 video / wmav2 audio). Same in-process pattern
   as the playback DSSourceFilter (libmpv replacing WMAsfReader).
   This kills the qasf/wmvcore/qcap dependency chain for export. */
HRESULT mw_writer_create(IBaseFilter_DS **ppFilter);

#endif
