#ifndef TRACE_H
#define TRACE_H

#include "log.h"

/* Trace every COM method entry. Usage: TRACE_ENTER; at the top of each method.
   Logs the function name and 'this' pointer automatically. */
#define TRACE_ENTER \
    proxy_log("[TRACE] %s (this=%p)", __FUNCTION__, (void*)(*(DWORD*)&This ? This : 0))

#define TRACE_METHOD(obj, name) \
    proxy_log("[TRACE] %s::%s (this=%p)", obj, name, (void*)This)

#define TRACE_HR(hr) \
    proxy_log("[TRACE]   -> hr=0x%08lX", (unsigned long)(hr))

#define TRACE_PTR(name, ptr) \
    proxy_log("[TRACE]   %s=%p", name, (void*)(ptr))

#define TRACE_MSG(fmt, ...) \
    proxy_log("[TRACE]   " fmt, ##__VA_ARGS__)

#endif
