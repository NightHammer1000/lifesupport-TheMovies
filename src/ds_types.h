#ifndef DS_TYPES_H
#define DS_TYPES_H

#include "wm_types.h"

/* AM_MEDIA_TYPE is identical to WM_MEDIA_TYPE */
typedef WM_MEDIA_TYPE AM_MEDIA_TYPE;

/* ========== Enums ========== */

typedef enum _FilterState {
    State_Stopped = 0,
    State_Paused  = 1,
    State_Running = 2
} FILTER_STATE;

typedef enum _PinDirection {
    PINDIR_INPUT  = 0,
    PINDIR_OUTPUT = 1
} PIN_DIRECTION;

/* Reference time: 100ns units */
typedef LONGLONG REFERENCE_TIME;

/* AM_SEEKING */
#define AM_SEEKING_CanSeekAbsolute    0x001
#define AM_SEEKING_CanSeekForwards    0x002
#define AM_SEEKING_CanSeekBackwards   0x004
#define AM_SEEKING_CanGetCurrentPos   0x008
#define AM_SEEKING_CanGetStopPos      0x010
#define AM_SEEKING_CanGetDuration     0x020

#define AM_SEEKING_AbsolutePositioning  0x0
#define AM_SEEKING_RelativePositioning  0x1
#define AM_SEEKING_IncrementalPositioning 0x2
#define AM_SEEKING_NoPositioning        0x0

/* {7CE85320-8943-11CD-8949-00A004BBCFAB} */
static const GUID TIME_FORMAT_MEDIA_TIME =
    {0x7CE85320, 0x8943, 0x11CD, {0x89,0x49,0x00,0xA0,0x04,0xBB,0xCF,0xAB}};

/* EC_COMPLETE */
#define EC_COMPLETE 0x01

/* ========== CLSIDs ========== */

/* {E436EBB3-524F-11CE-9F53-0020AF0BA770} */
static const GUID CLSID_FilterGraph =
    {0xE436EBB3, 0x524F, 0x11CE, {0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {187463A0-5BB7-11D3-ACBE-0080C75E246E} */
static const GUID CLSID_WMAsfReader =
    {0x187463A0, 0x5BB7, 0x11D3, {0xAC,0xBE,0x00,0x80,0xC7,0x5E,0x24,0x6E}};

/* ========== IIDs ========== */

/* {56A86895-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IBaseFilter =
    {0x56A86895, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A86899-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMediaFilter =
    {0x56A86899, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* IID_IPersist is already defined in system headers (objidl.h) */

/* {56A86891-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IPin =
    {0x56A86891, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A86892-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IEnumPins =
    {0x56A86892, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {89C31040-846B-11CE-97D3-00AA0055595A} */
static const GUID IID_IEnumMediaTypes =
    {0x89C31040, 0x846B, 0x11CE, {0x97,0xD3,0x00,0xAA,0x00,0x55,0x59,0x5A}};

/* {56A868A6-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IFileSourceFilter =
    {0x56A868A6, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A8689D-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMemInputPin =
    {0x56A8689D, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A8689C-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMemAllocator =
    {0x56A8689C, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A8689A-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMediaSample =
    {0x56A8689A, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A868B2-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMediaSeeking =
    {0x56A868B2, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A868A5-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IQualityControl =
    {0x56A868A5, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A868A9-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IGraphBuilder =
    {0x56A868A9, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A868B3-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IBasicAudio =
    {0x56A868B3, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {56A868B6-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IMediaEvent =
    {0x56A868B6, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* {36B73880-C2C8-11CF-8B46-00805F6CEF60} */
static const GUID IID_IMediaPosition =
    {0x36B73880, 0xC2C8, 0x11CF, {0x8B,0x46,0x00,0x80,0x5F,0x6C,0xEF,0x60}};

/* {56A86897-0AD4-11CE-B03A-0020AF0BA770} */
static const GUID IID_IReferenceClock =
    {0x56A86897, 0x0AD4, 0x11CE, {0xB0,0x3A,0x00,0x20,0xAF,0x0B,0xA7,0x70}};

/* ========== Structs ========== */

typedef struct _AllocatorProperties {
    long cBuffers;
    long cbBuffer;
    long cbAlign;
    long cbPrefix;
} ALLOCATOR_PROPERTIES;

/* Forward declarations */
typedef struct IBaseFilter_DS IBaseFilter_DS;
typedef struct IPin_DS IPin_DS;
typedef struct IEnumPins_DS IEnumPins_DS;
typedef struct IEnumMediaTypes_DS IEnumMediaTypes_DS;
typedef struct IFileSourceFilter_DS IFileSourceFilter_DS;
typedef struct IMemInputPin_DS IMemInputPin_DS;
typedef struct IMemAllocator_DS IMemAllocator_DS;
typedef struct IMediaSample_DS IMediaSample_DS;
typedef struct IMediaSeeking_DS IMediaSeeking_DS;
typedef struct IQualityControl_DS IQualityControl_DS;
typedef struct IFilterGraph_DS IFilterGraph_DS;
typedef struct IReferenceClock_DS IReferenceClock_DS;

/* PIN_INFO */
typedef struct _PinInfo {
    IBaseFilter_DS *pFilter;
    PIN_DIRECTION   dir;
    WCHAR           achName[128];
} PIN_INFO;

/* FILTER_INFO */
typedef struct _FilterInfo {
    WCHAR            achName[128];
    IFilterGraph_DS *pGraph;
} FILTER_INFO;

/* ========== IEnumPins ========== */

typedef struct IEnumPinsVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IEnumPins_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IEnumPins_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IEnumPins_DS*);
    HRESULT (STDMETHODCALLTYPE *Next)(IEnumPins_DS*, ULONG, IPin_DS**, ULONG*);
    HRESULT (STDMETHODCALLTYPE *Skip)(IEnumPins_DS*, ULONG);
    HRESULT (STDMETHODCALLTYPE *Reset)(IEnumPins_DS*);
    HRESULT (STDMETHODCALLTYPE *Clone)(IEnumPins_DS*, IEnumPins_DS**);
} IEnumPinsVtbl_DS;
struct IEnumPins_DS { const IEnumPinsVtbl_DS *lpVtbl; };

/* ========== IEnumMediaTypes ========== */

typedef struct IEnumMediaTypesVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IEnumMediaTypes_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IEnumMediaTypes_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IEnumMediaTypes_DS*);
    HRESULT (STDMETHODCALLTYPE *Next)(IEnumMediaTypes_DS*, ULONG, AM_MEDIA_TYPE**, ULONG*);
    HRESULT (STDMETHODCALLTYPE *Skip)(IEnumMediaTypes_DS*, ULONG);
    HRESULT (STDMETHODCALLTYPE *Reset)(IEnumMediaTypes_DS*);
    HRESULT (STDMETHODCALLTYPE *Clone)(IEnumMediaTypes_DS*, IEnumMediaTypes_DS**);
} IEnumMediaTypesVtbl_DS;
struct IEnumMediaTypes_DS { const IEnumMediaTypesVtbl_DS *lpVtbl; };

/* ========== IPin ========== */

typedef struct IPinVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IPin_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IPin_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IPin_DS*);
    HRESULT (STDMETHODCALLTYPE *Connect)(IPin_DS*, IPin_DS*, const AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *ReceiveConnection)(IPin_DS*, IPin_DS*, const AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *Disconnect)(IPin_DS*);
    HRESULT (STDMETHODCALLTYPE *ConnectedTo)(IPin_DS*, IPin_DS**);
    HRESULT (STDMETHODCALLTYPE *ConnectionMediaType)(IPin_DS*, AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *QueryPinInfo)(IPin_DS*, PIN_INFO*);
    HRESULT (STDMETHODCALLTYPE *QueryDirection)(IPin_DS*, PIN_DIRECTION*);
    HRESULT (STDMETHODCALLTYPE *QueryId)(IPin_DS*, LPWSTR*);
    HRESULT (STDMETHODCALLTYPE *QueryAccept)(IPin_DS*, const AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *EnumMediaTypes)(IPin_DS*, IEnumMediaTypes_DS**);
    HRESULT (STDMETHODCALLTYPE *QueryInternalConnections)(IPin_DS*, IPin_DS**, ULONG*);
    HRESULT (STDMETHODCALLTYPE *EndOfStream)(IPin_DS*);
    HRESULT (STDMETHODCALLTYPE *BeginFlush)(IPin_DS*);
    HRESULT (STDMETHODCALLTYPE *EndFlush)(IPin_DS*);
    HRESULT (STDMETHODCALLTYPE *NewSegment)(IPin_DS*, REFERENCE_TIME, REFERENCE_TIME, double);
} IPinVtbl_DS;
struct IPin_DS { const IPinVtbl_DS *lpVtbl; };

/* ========== IMemInputPin ========== */

typedef struct IMemInputPinVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMemInputPin_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMemInputPin_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IMemInputPin_DS*);
    HRESULT (STDMETHODCALLTYPE *GetAllocator)(IMemInputPin_DS*, IMemAllocator_DS**);
    HRESULT (STDMETHODCALLTYPE *NotifyAllocator)(IMemInputPin_DS*, IMemAllocator_DS*, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetAllocatorRequirements)(IMemInputPin_DS*, ALLOCATOR_PROPERTIES*);
    HRESULT (STDMETHODCALLTYPE *Receive)(IMemInputPin_DS*, IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *ReceiveMultiple)(IMemInputPin_DS*, IMediaSample_DS**, long, long*);
    HRESULT (STDMETHODCALLTYPE *ReceiveCanBlock)(IMemInputPin_DS*);
} IMemInputPinVtbl_DS;
struct IMemInputPin_DS { const IMemInputPinVtbl_DS *lpVtbl; };

/* ========== IMemAllocator ========== */

typedef struct IMemAllocatorVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMemAllocator_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMemAllocator_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IMemAllocator_DS*);
    HRESULT (STDMETHODCALLTYPE *SetProperties)(IMemAllocator_DS*, ALLOCATOR_PROPERTIES*, ALLOCATOR_PROPERTIES*);
    HRESULT (STDMETHODCALLTYPE *GetProperties)(IMemAllocator_DS*, ALLOCATOR_PROPERTIES*);
    HRESULT (STDMETHODCALLTYPE *Commit)(IMemAllocator_DS*);
    HRESULT (STDMETHODCALLTYPE *Decommit)(IMemAllocator_DS*);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(IMemAllocator_DS*, IMediaSample_DS**, REFERENCE_TIME*, REFERENCE_TIME*, DWORD);
    HRESULT (STDMETHODCALLTYPE *ReleaseBuffer)(IMemAllocator_DS*, IMediaSample_DS*);
} IMemAllocatorVtbl_DS;
struct IMemAllocator_DS { const IMemAllocatorVtbl_DS *lpVtbl; };

/* ========== IMediaSample ========== */

typedef struct IMediaSampleVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMediaSample_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMediaSample_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *GetPointer)(IMediaSample_DS*, BYTE**);
    long    (STDMETHODCALLTYPE *GetSize)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *GetTime)(IMediaSample_DS*, REFERENCE_TIME*, REFERENCE_TIME*);
    HRESULT (STDMETHODCALLTYPE *SetTime)(IMediaSample_DS*, REFERENCE_TIME*, REFERENCE_TIME*);
    HRESULT (STDMETHODCALLTYPE *IsSyncPoint)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *SetSyncPoint)(IMediaSample_DS*, BOOL);
    HRESULT (STDMETHODCALLTYPE *IsPreroll)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *SetPreroll)(IMediaSample_DS*, BOOL);
    long    (STDMETHODCALLTYPE *GetActualDataLength)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *SetActualDataLength)(IMediaSample_DS*, long);
    HRESULT (STDMETHODCALLTYPE *GetMediaType)(IMediaSample_DS*, AM_MEDIA_TYPE**);
    HRESULT (STDMETHODCALLTYPE *SetMediaType)(IMediaSample_DS*, AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *IsDiscontinuity)(IMediaSample_DS*);
    HRESULT (STDMETHODCALLTYPE *SetDiscontinuity)(IMediaSample_DS*, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetMediaTime)(IMediaSample_DS*, LONGLONG*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *SetMediaTime)(IMediaSample_DS*, LONGLONG*, LONGLONG*);
} IMediaSampleVtbl_DS;
struct IMediaSample_DS { const IMediaSampleVtbl_DS *lpVtbl; };

/* ========== IBaseFilter ========== */

typedef struct IBaseFilterVtbl_DS {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IBaseFilter_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IBaseFilter_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IBaseFilter_DS*);
    /* IPersist */
    HRESULT (STDMETHODCALLTYPE *GetClassID)(IBaseFilter_DS*, CLSID*);
    /* IMediaFilter */
    HRESULT (STDMETHODCALLTYPE *Stop)(IBaseFilter_DS*);
    HRESULT (STDMETHODCALLTYPE *Pause)(IBaseFilter_DS*);
    HRESULT (STDMETHODCALLTYPE *Run)(IBaseFilter_DS*, REFERENCE_TIME);
    HRESULT (STDMETHODCALLTYPE *GetState)(IBaseFilter_DS*, DWORD, FILTER_STATE*);
    HRESULT (STDMETHODCALLTYPE *SetSyncSource)(IBaseFilter_DS*, IReferenceClock_DS*);
    HRESULT (STDMETHODCALLTYPE *GetSyncSource)(IBaseFilter_DS*, IReferenceClock_DS**);
    /* IBaseFilter */
    HRESULT (STDMETHODCALLTYPE *EnumPins)(IBaseFilter_DS*, IEnumPins_DS**);
    HRESULT (STDMETHODCALLTYPE *FindPin)(IBaseFilter_DS*, LPCWSTR, IPin_DS**);
    HRESULT (STDMETHODCALLTYPE *QueryFilterInfo)(IBaseFilter_DS*, FILTER_INFO*);
    HRESULT (STDMETHODCALLTYPE *JoinFilterGraph)(IBaseFilter_DS*, IFilterGraph_DS*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *QueryVendorInfo)(IBaseFilter_DS*, LPWSTR*);
} IBaseFilterVtbl_DS;
struct IBaseFilter_DS { const IBaseFilterVtbl_DS *lpVtbl; };

/* ========== IFileSourceFilter ========== */

typedef struct IFileSourceFilterVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IFileSourceFilter_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IFileSourceFilter_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IFileSourceFilter_DS*);
    HRESULT (STDMETHODCALLTYPE *Load)(IFileSourceFilter_DS*, LPCOLESTR, const AM_MEDIA_TYPE*);
    HRESULT (STDMETHODCALLTYPE *GetCurFile)(IFileSourceFilter_DS*, LPOLESTR*, AM_MEDIA_TYPE*);
} IFileSourceFilterVtbl_DS;
struct IFileSourceFilter_DS { const IFileSourceFilterVtbl_DS *lpVtbl; };

/* ========== IMediaSeeking ========== */

typedef struct IMediaSeekingVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMediaSeeking_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMediaSeeking_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IMediaSeeking_DS*);
    HRESULT (STDMETHODCALLTYPE *GetCapabilities)(IMediaSeeking_DS*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *CheckCapabilities)(IMediaSeeking_DS*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *IsFormatSupported)(IMediaSeeking_DS*, const GUID*);
    HRESULT (STDMETHODCALLTYPE *QueryPreferredFormat)(IMediaSeeking_DS*, GUID*);
    HRESULT (STDMETHODCALLTYPE *GetTimeFormat)(IMediaSeeking_DS*, GUID*);
    HRESULT (STDMETHODCALLTYPE *IsUsingTimeFormat)(IMediaSeeking_DS*, const GUID*);
    HRESULT (STDMETHODCALLTYPE *SetTimeFormat)(IMediaSeeking_DS*, const GUID*);
    HRESULT (STDMETHODCALLTYPE *GetDuration)(IMediaSeeking_DS*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *GetStopPosition)(IMediaSeeking_DS*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPosition)(IMediaSeeking_DS*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *ConvertTimeFormat)(IMediaSeeking_DS*, LONGLONG*, const GUID*, LONGLONG, const GUID*);
    HRESULT (STDMETHODCALLTYPE *SetPositions)(IMediaSeeking_DS*, LONGLONG*, DWORD, LONGLONG*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetPositions)(IMediaSeeking_DS*, LONGLONG*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *GetAvailable)(IMediaSeeking_DS*, LONGLONG*, LONGLONG*);
    HRESULT (STDMETHODCALLTYPE *SetRate)(IMediaSeeking_DS*, double);
    HRESULT (STDMETHODCALLTYPE *GetRate)(IMediaSeeking_DS*, double*);
    HRESULT (STDMETHODCALLTYPE *GetPreroll)(IMediaSeeking_DS*, LONGLONG*);
} IMediaSeekingVtbl_DS;
struct IMediaSeeking_DS { const IMediaSeekingVtbl_DS *lpVtbl; };

/* ========== IQualityControl ========== */

typedef struct IQualityControlVtbl_DS {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IQualityControl_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IQualityControl_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IQualityControl_DS*);
    HRESULT (STDMETHODCALLTYPE *Notify)(IQualityControl_DS*, IBaseFilter_DS*, int /* Quality */);
    HRESULT (STDMETHODCALLTYPE *SetSink)(IQualityControl_DS*, IQualityControl_DS*);
} IQualityControlVtbl_DS;
struct IQualityControl_DS { const IQualityControlVtbl_DS *lpVtbl; };

/* ========== IBasicAudio (IDispatch + 4 methods) ========== */

typedef struct IBasicAudio_DS IBasicAudio_DS;
typedef struct IBasicAudioVtbl_DS {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IBasicAudio_DS*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IBasicAudio_DS*);
    ULONG   (STDMETHODCALLTYPE *Release)(IBasicAudio_DS*);
    /* IDispatch */
    HRESULT (STDMETHODCALLTYPE *GetTypeInfoCount)(IBasicAudio_DS*, UINT*);
    HRESULT (STDMETHODCALLTYPE *GetTypeInfo)(IBasicAudio_DS*, UINT, LCID, void**);
    HRESULT (STDMETHODCALLTYPE *GetIDsOfNames)(IBasicAudio_DS*, REFIID, LPOLESTR*, UINT, LCID, DISPID*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(IBasicAudio_DS*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*);
    /* IBasicAudio */
    HRESULT (STDMETHODCALLTYPE *put_Volume)(IBasicAudio_DS*, long);
    HRESULT (STDMETHODCALLTYPE *get_Volume)(IBasicAudio_DS*, long*);
    HRESULT (STDMETHODCALLTYPE *put_Balance)(IBasicAudio_DS*, long);
    HRESULT (STDMETHODCALLTYPE *get_Balance)(IBasicAudio_DS*, long*);
} IBasicAudioVtbl_DS;
struct IBasicAudio_DS { const IBasicAudioVtbl_DS *lpVtbl; };

/* ========== Minimal IFilterGraph (for JoinFilterGraph) ========== */

struct IFilterGraph_DS { const void *lpVtbl; };
struct IReferenceClock_DS { const void *lpVtbl; };

/* ========== Helpers ========== */

/* VFW error codes */
#ifndef VFW_E_NOT_CONNECTED
#define VFW_E_NOT_CONNECTED      ((HRESULT)0x80040209L)
#endif
#ifndef VFW_E_NOT_FOUND
#define VFW_E_NOT_FOUND          ((HRESULT)0x80040216L)
#endif
#ifndef VFW_E_TYPE_NOT_ACCEPTED
#define VFW_E_TYPE_NOT_ACCEPTED  ((HRESULT)0x8004022AL)
#endif

#define VHR_FAILED(hr) ((HRESULT)(hr) < 0)

static inline AM_MEDIA_TYPE *media_type_clone(const AM_MEDIA_TYPE *src) {
    if (!src) return NULL;
    AM_MEDIA_TYPE *dst = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!dst) return NULL;
    *dst = *src;
    dst->pUnk = NULL;
    if (src->cbFormat > 0 && src->pbFormat) {
        dst->pbFormat = (BYTE *)CoTaskMemAlloc(src->cbFormat);
        if (dst->pbFormat) memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    return dst;
}

static inline void media_type_free(AM_MEDIA_TYPE *mt) {
    if (!mt) return;
    if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
    CoTaskMemFree(mt);
}

#endif /* DS_TYPES_H */
