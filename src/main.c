#include "log.h"
#include "sync_reader.h"
#include "profile_mgr.h"
#include "ds_filter.h"
#include "asf_writer.h"
#include "ds_fakegraph.h"

#include <MinHook.h>

/* ================================================================
 * WMF function signatures (what the real wmvcore.dll exports)
 * ================================================================ */
typedef HRESULT (WINAPI *pfn_WMCreateSyncReader)(IUnknown *pUnkCert, DWORD dwRights, IWMSyncReader **ppSyncReader);
typedef HRESULT (WINAPI *pfn_WMCreateProfileManager)(IWMProfileManager **ppProfileManager);
typedef HMODULE (WINAPI *pfn_LoadLibraryA)(LPCSTR lpLibFileName);
typedef HMODULE (WINAPI *pfn_LoadLibraryW)(LPCWSTR lpLibFileName);
typedef FARPROC (WINAPI *pfn_GetProcAddress)(HMODULE hModule, LPCSTR lpProcName);

typedef HRESULT (WINAPI *pfn_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);

/* Original function pointers (set by MinHook) */
static pfn_LoadLibraryA   orig_LoadLibraryA   = NULL;
static pfn_LoadLibraryW   orig_LoadLibraryW   = NULL;
static pfn_GetProcAddress orig_GetProcAddress  = NULL;
static pfn_CoCreateInstance orig_CoCreateInstance = NULL;

/* Our fake module handle for wmvcore — just a sentinel value */
static HMODULE g_fake_wmvcore = (HMODULE)0xDEAD0001;

/* ================================================================
 * The two WMF functions we provide
 * ================================================================ */

static HRESULT WINAPI Proxy_WMCreateSyncReader(IUnknown *pUnkCert, DWORD dwRights, IWMSyncReader **ppReader) {
    proxy_log("WMCreateSyncReader(cert=%p, rights=0x%lX)", pUnkCert, dwRights);
    return proxy_sync_reader_create(ppReader);
}

static HRESULT WINAPI Proxy_WMCreateProfileManager(IWMProfileManager **ppMgr) {
    proxy_log("WMCreateProfileManager()");
    return proxy_profile_manager_create(ppMgr);
}

/* ================================================================
 * LoadLibrary hooks — intercept wmvcore.dll loading
 * ================================================================ */

static BOOL is_wmvcore(const char *name) {
    if (!name) return FALSE;
    /* Extract filename from full path */
    const char *slash = strrchr(name, '\\');
    const char *fslash = strrchr(name, '/');
    if (fslash > slash) slash = fslash;
    const char *fname = slash ? slash + 1 : name;
    return (_stricmp(fname, "wmvcore.dll") == 0);
}

static BOOL is_wmvcore_w(const WCHAR *name) {
    if (!name) return FALSE;
    const WCHAR *slash = wcsrchr(name, L'\\');
    const WCHAR *fslash = wcsrchr(name, L'/');
    if (fslash > slash) slash = fslash;
    const WCHAR *fname = slash ? slash + 1 : name;
    return (_wcsicmp(fname, L"wmvcore.dll") == 0);
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    if (is_wmvcore(lpLibFileName)) {
        proxy_log("LoadLibraryA(\"%s\") -> intercepted", lpLibFileName);
        return g_fake_wmvcore;
    }
    return orig_LoadLibraryA(lpLibFileName);
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName) {
    if (is_wmvcore_w(lpLibFileName)) {
        proxy_log("LoadLibraryW(\"wmvcore.dll\") -> intercepted");
        return g_fake_wmvcore;
    }
    return orig_LoadLibraryW(lpLibFileName);
}

/* ================================================================
 * GetProcAddress hook — return our functions for the fake handle
 * ================================================================ */

static FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (hModule == g_fake_wmvcore && lpProcName) {
        if (strcmp(lpProcName, "WMCreateSyncReader") == 0) {
            proxy_log("GetProcAddress(wmvcore, \"WMCreateSyncReader\") -> proxy");
            return (FARPROC)Proxy_WMCreateSyncReader;
        }
        if (strcmp(lpProcName, "WMCreateProfileManager") == 0) {
            proxy_log("GetProcAddress(wmvcore, \"WMCreateProfileManager\") -> proxy");
            return (FARPROC)Proxy_WMCreateProfileManager;
        }
        proxy_log("GetProcAddress(wmvcore, \"%s\") -> NULL (unimplemented)", lpProcName);
        return NULL;
    }
    return orig_GetProcAddress(hModule, lpProcName);
}

/* ================================================================
 * CoCreateInstance hook — intercept FilterGraph + WM ASF Reader
 * ================================================================ */

static BOOL g_in_video_setup = FALSE; /* TRUE between FilterGraph and WMAsfReader creation */

/* Map a CLSID to a short human-readable name (best-effort) for the log. */
static const char *clsid_name(REFCLSID rclsid) {
    if (IsEqualGUID(rclsid, &CLSID_FilterGraph))           return "CLSID_FilterGraph";
    if (IsEqualGUID(rclsid, &CLSID_WMAsfReader))           return "CLSID_WMAsfReader";
    if (IsEqualGUID(rclsid, &CLSID_CaptureGraphBuilder2))  return "CLSID_CaptureGraphBuilder2";
    if (IsEqualGUID(rclsid, &CLSID_WMAsfWriter))           return "CLSID_WMAsfWriter";
    if (IsEqualGUID(rclsid, &CLSID_AsyncReader))           return "CLSID_AsyncReader";
    if (IsEqualGUID(rclsid, &CLSID_WaveParser))            return "CLSID_WaveParser";
    return NULL;
}

static HRESULT WINAPI Hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter,
    DWORD dwClsContext, REFIID riid, LPVOID *ppv)
{
    /* Intercept FilterGraph — return our fake graph that bypasses quartz.dll */
    if (IsEqualGUID(rclsid, &CLSID_FilterGraph)) {
        proxy_log("CoCreateInstance(CLSID_FilterGraph) -> FakeGraph");
        g_in_video_setup = TRUE;
        return ds_fakegraph_create(ppv);
    }

    /* Intercept WM ASF Reader — return our FFmpeg source filter */
    if (IsEqualGUID(rclsid, &CLSID_WMAsfReader)) {
        proxy_log("CoCreateInstance(CLSID_WMAsfReader)");
        IBaseFilter_DS *pFilter = NULL;
        HRESULT hr = ds_source_filter_create(&pFilter);
        if (hr < 0) { proxy_log("  create failed: 0x%08lX", hr); return hr; }
        hr = pFilter->lpVtbl->QueryInterface(pFilter, riid, ppv);
        pFilter->lpVtbl->Release(pFilter);
        g_in_video_setup = FALSE;
        return hr;
    }

    /* Intercept WM ASF Writer — return our libavformat-backed writer.
       Same pattern as WMAsfReader → DSSourceFilter (libmpv): we
       short-circuit qasf.dll entirely so export doesn't depend on the
       Microsoft component being present or correct on the host. */
    if (IsEqualGUID(rclsid, &CLSID_WMAsfWriter)) {
        proxy_log("CoCreateInstance(CLSID_WMAsfWriter) -> MoviesAsfWriter");
        IBaseFilter_DS *pFilter = NULL;
        HRESULT hr = mw_writer_create(&pFilter);
        if (hr < 0) { proxy_log("  create failed: 0x%08lX", hr); return hr; }
        hr = pFilter->lpVtbl->QueryInterface(pFilter, riid, ppv);
        pFilter->lpVtbl->Release(pFilter);
        return hr;
    }

    /* Other writer-path CLSIDs (CGB2, AsyncReader, WaveParser) still pass
       through. CGB2 just calls back into our IGraphBuilder, which is
       fine. */
    const char *known = clsid_name(rclsid);
    if (known &&
        (IsEqualGUID(rclsid, &CLSID_CaptureGraphBuilder2) ||
         IsEqualGUID(rclsid, &CLSID_AsyncReader) ||
         IsEqualGUID(rclsid, &CLSID_WaveParser))) {
        proxy_log("CoCreateInstance(%s, riid={%08lX-%04X-%04X}) — passthrough",
                  known, riid->Data1, riid->Data2, riid->Data3);
        HRESULT hr = orig_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
        proxy_log("  -> hr=0x%08lX, ppv=%p", hr, ppv ? *ppv : NULL);
        return hr;
    }

    /* Everything else passes through to real CoCreateInstance */
    HRESULT hr = orig_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    return hr;
}

/* ================================================================
 * Hook installation / removal
 *
 * All production hooks target Windows APIs (LoadLibrary*, GetProcAddress,
 * CoCreateInstance) — no game-binary addresses. The mod stays version-
 * agnostic across MoviesSE.exe revisions.
 * ================================================================ */

static BOOL install_hooks(void) {
    if (MH_Initialize() != MH_OK) {
        proxy_log("MH_Initialize failed");
        return FALSE;
    }

    MH_STATUS s;

    s = MH_CreateHookApi(L"kernel32", "LoadLibraryA", (LPVOID)Hook_LoadLibraryA, (LPVOID *)&orig_LoadLibraryA);
    if (s != MH_OK) { proxy_log("Hook LoadLibraryA failed: %d", s); return FALSE; }

    s = MH_CreateHookApi(L"kernel32", "LoadLibraryW", (LPVOID)Hook_LoadLibraryW, (LPVOID *)&orig_LoadLibraryW);
    if (s != MH_OK) { proxy_log("Hook LoadLibraryW failed: %d", s); return FALSE; }

    s = MH_CreateHookApi(L"kernel32", "GetProcAddress", (LPVOID)Hook_GetProcAddress, (LPVOID *)&orig_GetProcAddress);
    if (s != MH_OK) { proxy_log("Hook GetProcAddress failed: %d", s); return FALSE; }

    s = MH_CreateHookApi(L"ole32", "CoCreateInstance", (LPVOID)Hook_CoCreateInstance, (LPVOID *)&orig_CoCreateInstance);
    if (s != MH_OK) { proxy_log("Hook CoCreateInstance failed: %d", s); return FALSE; }

    s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) { proxy_log("MH_EnableHook failed: %d", s); return FALSE; }

    proxy_log("All hooks installed successfully");
    return TRUE;
}

static void remove_hooks(void) {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

/* ================================================================
 * Crash catcher — logs registers and stack on access violation
 * ================================================================ */

static volatile LONG g_crash_count = 0;

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD ec = ep->ExceptionRecord->ExceptionCode;
    /* Catch fatal hardware exceptions wherever they happen (system DLLs
       included). Filtering by module hid writer-path crashes that happen
       deep inside qedit / wmvcore — the log just stopped, looking like a
       silent exit. Skip noisy software exceptions (C++ throws, RPC) since
       those are routinely SEH-handled. */
    BOOL fatal = (ec == EXCEPTION_ACCESS_VIOLATION ||
                  ec == EXCEPTION_STACK_OVERFLOW ||
                  ec == EXCEPTION_ILLEGAL_INSTRUCTION ||
                  ec == EXCEPTION_PRIV_INSTRUCTION ||
                  ec == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                  ec == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
                  ec == EXCEPTION_DATATYPE_MISALIGNMENT);
    if (fatal) {
        LONG count = InterlockedIncrement(&g_crash_count);
        if (count > 5) return EXCEPTION_CONTINUE_SEARCH; /* limit logging */
        CONTEXT *c = ep->ContextRecord;
        proxy_log("!!! CRASH #%ld: code=0x%08lX at EIP=0x%08lX (thread=%lu) !!!",
                  count, ec, c->Eip, GetCurrentThreadId());
        proxy_log("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
                  c->Eax, c->Ebx, c->Ecx, c->Edx);
        proxy_log("  ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
                  c->Esi, c->Edi, c->Ebp, c->Esp);
        proxy_log("  ExceptionAddr=%p Read/Write=%lu TargetAddr=0x%08lX",
                  ep->ExceptionRecord->ExceptionAddress,
                  ep->ExceptionRecord->ExceptionInformation[0],
                  (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);

        /* Dump stack */
        DWORD *stack = (DWORD *)(ULONG_PTR)c->Esp;
        proxy_log("  Stack:");
        for (int i = 0; i < 16; i += 4) {
            proxy_log("    ESP+%02X: %08lX %08lX %08lX %08lX",
                      i*4, stack[i], stack[i+1], stack[i+2], stack[i+3]);
        }

        /* Resolve every stack word that points into a loaded module — gives
           us a poor-man's stack trace without needing StackWalk64. The crash
           cause is almost always one of the return addresses on the stack
           when EIP itself is inside a system DLL (USER32 et al). */
        proxy_log("  Stack-word module resolution (first 128 DWORDs):");
        for (int i = 0; i < 128; i++) {
            if (IsBadReadPtr(&stack[i], 4)) break;
            DWORD v = stack[i];
            if (v < 0x10000 || v >= 0x80000000) continue;  /* skip non-code-looking values */
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(ULONG_PTR)v, &m) && m) {
                char modname[MAX_PATH] = {0};
                GetModuleFileNameA(m, modname, sizeof(modname));
                const char *base = strrchr(modname, '\\');
                base = base ? base + 1 : modname;
                proxy_log("    ESP+%03X: %08lX  %s+0x%lX",
                          i*4, v, base, v - (DWORD)(ULONG_PTR)m);
            }
        }

        /* Try to dump the object at EDI (likely the COM object being called) */
        DWORD *edi_obj = (DWORD *)(ULONG_PTR)c->Edi;
        if (edi_obj && !IsBadReadPtr(edi_obj, 32)) {
            proxy_log("  Object at EDI (0x%08lX):", c->Edi);
            proxy_log("    [0]=%08lX [1]=%08lX [2]=%08lX [3]=%08lX",
                      edi_obj[0], edi_obj[1], edi_obj[2], edi_obj[3]);
            /* If vtable ptr is valid, dump vtable entries */
            DWORD *vtbl = (DWORD *)(ULONG_PTR)edi_obj[0];
            if (vtbl && !IsBadReadPtr(vtbl, 64)) {
                proxy_log("    vtbl[0]=%08lX [1]=%08lX [2]=%08lX [3]=%08lX [4]=%08lX [5]=%08lX [6]=%08lX [7]=%08lX",
                          vtbl[0], vtbl[1], vtbl[2], vtbl[3], vtbl[4], vtbl[5], vtbl[6], vtbl[7]);
            }
        }

        /* Try to trace back: what was the caller doing? Read code before the crash EIP */
        BYTE *code_at_return = (BYTE *)(ULONG_PTR)stack[0];
        if (code_at_return && !IsBadReadPtr(code_at_return - 16, 32)) {
            proxy_log("  Code around return addr (ESP[0]=0x%08lX):", stack[0]);
            BYTE *p = code_at_return - 16;
            proxy_log("    %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X",
                      p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                      p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
        }

        /* Dump 32 bytes around EIP — instruction fingerprint of the USER32
           function we crashed in. The pipe marks the crash site itself
           (bytes at EIP), 16 bytes of context on each side. */
        BYTE *eip_bytes = (BYTE *)(ULONG_PTR)c->Eip;
        if (eip_bytes && !IsBadReadPtr(eip_bytes - 16, 48)) {
            BYTE *p = eip_bytes - 16;
            proxy_log("  Code around EIP (0x%08lX):", c->Eip);
            proxy_log("    %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
                      "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],
                      p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23],p[24],p[25],p[26],p[27],p[28],p[29],p[30],p[31]);
        }

        /* Find what module 0xABCFBB04 is in */
        HMODULE hMod = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(ULONG_PTR)c->Eip, &hMod)) {
            char modname[MAX_PATH];
            GetModuleFileNameA(hMod, modname, MAX_PATH);
            proxy_log("  EIP is in module: %s (base=%p)", modname, hMod);
        } else {
            proxy_log("  EIP 0x%08lX is NOT in any loaded module", c->Eip);
        }

        /* Check what module the quartz caller is in */
        DWORD quartz_addr = stack[12]; /* ESP+30 area */
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(ULONG_PTR)quartz_addr, &hMod)) {
            char modname[MAX_PATH];
            GetModuleFileNameA(hMod, modname, MAX_PATH);
            proxy_log("  Stack caller 0x%08lX in module: %s (base=%p)", quartz_addr, modname, hMod);
        }

        /* Also dump ESP-based frame for caller identification */
        DWORD *outer_stack = (DWORD *)(ULONG_PTR)(c->Esp + 0x30);
        if (!IsBadReadPtr(outer_stack, 32)) {
            proxy_log("  Outer stack (ESP+30):");
            proxy_log("    %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                      outer_stack[0], outer_stack[1], outer_stack[2], outer_stack[3],
                      outer_stack[4], outer_stack[5], outer_stack[6], outer_stack[7]);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ================================================================
 * DllMain — ASI entry point
 * ================================================================ */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        log_init();
        AddVectoredExceptionHandler(1, crash_handler);
        proxy_log("=== movies_fix.asi loaded ===");
        proxy_log("Build: " __DATE__ " " __TIME__);
        if (!install_hooks()) {
            proxy_log("FATAL: hook installation failed");
        }
        break;

    case DLL_PROCESS_DETACH:
        proxy_log("=== movies_fix.asi unloading ===");
        remove_hooks();
        log_close();
        break;
    }
    return TRUE;
}
