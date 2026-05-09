#ifndef DS_FAKEGRAPH_H
#define DS_FAKEGRAPH_H

#include "ds_types.h"

/* Create a fake FilterGraph that bypasses quartz.dll entirely.
   Implements IGraphBuilder, IMediaControl, IMediaSeeking,
   IMediaPosition, IMediaEvent, IBasicAudio. */
HRESULT ds_fakegraph_create(void **ppGraph);

#endif
