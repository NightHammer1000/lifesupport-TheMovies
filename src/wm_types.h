#ifndef WM_TYPES_H
#define WM_TYPES_H

#define COBJMACROS
#include <windows.h>
#include <unknwn.h>
#include <objbase.h>
#include <mmreg.h>

#ifndef ASF_E_BUFFERTOOSMALL
#define ASF_E_BUFFERTOOSMALL  ((HRESULT)0xC00D0801L)
#endif
#ifndef NS_E_INVALID_REQUEST
#define NS_E_INVALID_REQUEST  ((HRESULT)0xC00D002BL)
#endif
#ifndef NS_E_INVALID_OUTPUT_FORMAT
#define NS_E_INVALID_OUTPUT_FORMAT ((HRESULT)0xC00D0020L)
#endif

typedef UINT64 QWORD;

typedef enum WMT_STREAM_SELECTION {
    WMT_OFF = 0,
    WMT_CLEANPOINT_ONLY = 1,
    WMT_ON = 2
} WMT_STREAM_SELECTION;

typedef enum WMT_ATTR_DATATYPE {
    WMT_TYPE_DWORD = 0,
    WMT_TYPE_STRING = 1,
    WMT_TYPE_BINARY = 2,
    WMT_TYPE_BOOL = 3,
    WMT_TYPE_QWORD = 4,
    WMT_TYPE_WORD = 5,
    WMT_TYPE_GUID = 6
} WMT_ATTR_DATATYPE;

typedef enum WMT_VERSION {
    WMT_VER_4_0 = 0x00040000,
    WMT_VER_7_0 = 0x00070000,
    WMT_VER_8_0 = 0x00080000,
    WMT_VER_9_0 = 0x00090000
} WMT_VERSION;

/* {73647561-0000-0010-8000-00AA00389B71} */
static const GUID WMMEDIATYPE_Audio =
    {0x73647561, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
/* {73646976-0000-0010-8000-00AA00389B71} */
static const GUID WMMEDIATYPE_Video =
    {0x73646976, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

/* Uncompressed subtypes */
static const GUID WMMEDIASUBTYPE_PCM =
    {0x00000001, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID WMMEDIASUBTYPE_RGB24 =
    {0xe436eb7d, 0x524f, 0x11ce, {0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID WMMEDIASUBTYPE_RGB32 =
    {0xe436eb7c, 0x524f, 0x11ce, {0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70}};

/* WMV3 video / WMA2 audio compressed subtypes (the codecs the game's
   export profiles request — see STATUS.md). */
static const GUID MEDIASUBTYPE_WMV3 =
    {0x33564D57, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID MEDIASUBTYPE_WMAUDIO2 =
    {0x00000161, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

/* Format type GUIDs */
static const GUID FORMAT_VideoInfo =
    {0x05589f80, 0xc356, 0x11ce, {0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static const GUID FORMAT_WaveFormatEx =
    {0x05589f81, 0xc356, 0x11ce, {0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};

/* WM_MEDIA_TYPE - identical layout to DirectShow AM_MEDIA_TYPE */
typedef struct WM_MEDIA_TYPE {
    GUID   majortype;
    GUID   subtype;
    BOOL   bFixedSizeSamples;
    BOOL   bTemporalCompression;
    ULONG  lSampleSize;
    GUID   formattype;
    IUnknown *pUnk;
    ULONG  cbFormat;
    BYTE  *pbFormat;
} WM_MEDIA_TYPE;

typedef struct VIDEOINFOHEADER {
    RECT             rcSource;
    RECT             rcTarget;
    DWORD            dwBitRate;
    DWORD            dwBitErrorRate;
    LONGLONG         AvgTimePerFrame;
    BITMAPINFOHEADER bmiHeader;
} VIDEOINFOHEADER;

/* ========== COM Interface IIDs ========== */

/* {E1CD3524-03D7-11d2-9EED-006097D2D7CF} */
static const GUID IID_INSSBuffer =
    {0xE1CD3524, 0x03D7, 0x11d2, {0x9E,0xED,0x00,0x60,0x97,0xD2,0xD7,0xCF}};

/* {96406BCE-2B2B-11d3-B36B-00C04F6108FF} */
static const GUID IID_IWMMediaProps =
    {0x96406BCE, 0x2B2B, 0x11d3, {0xB3,0x6B,0x00,0xC0,0x4F,0x61,0x08,0xFF}};

/* {96406BD7-2B2B-11d3-B36B-00C04F6108FF} */
static const GUID IID_IWMOutputMediaProps =
    {0x96406BD7, 0x2B2B, 0x11d3, {0xB3,0x6B,0x00,0xC0,0x4F,0x61,0x08,0xFF}};

/* {9397F121-7705-4dc9-B049-98B698188414} */
static const GUID IID_IWMSyncReader =
    {0x9397F121, 0x7705, 0x4dc9, {0xB0,0x49,0x98,0xB6,0x98,0x18,0x84,0x14}};

/* {d16679f2-6ca0-472d-8d31-2f5d55aee155} */
static const GUID IID_IWMProfileManager =
    {0xd16679f2, 0x6ca0, 0x472d, {0x8d,0x31,0x2f,0x5d,0x55,0xae,0xe1,0x55}};

/* {96406BDB-2B2B-11d3-B36B-00C04F6108FF} */
static const GUID IID_IWMProfile =
    {0x96406BDB, 0x2B2B, 0x11d3, {0xB3,0x6B,0x00,0xC0,0x4F,0x61,0x08,0xFF}};

/* {96406BDC-2B2B-11d3-B36B-00C04F6108FF} */
static const GUID IID_IWMStreamConfig =
    {0x96406BDC, 0x2B2B, 0x11d3, {0xB3,0x6B,0x00,0xC0,0x4F,0x61,0x08,0xFF}};

/* {15CC68E3-27CC-4ECD-B222-3F5D02D80BD5} */
static const GUID IID_IWMHeaderInfo3 =
    {0x15CC68E3, 0x27CC, 0x4ECD, {0xB2,0x22,0x3F,0x5D,0x02,0xD8,0x0B,0xD5}};

/* {96406BDA-2B2B-11d3-B36B-00C04F6108FF} */
static const GUID IID_IWMHeaderInfo =
    {0x96406BDA, 0x2B2B, 0x11d3, {0xB3,0x6B,0x00,0xC0,0x4F,0x61,0x08,0xFF}};

/* ========== INSSBuffer ========== */

typedef struct INSSBuffer INSSBuffer;
typedef struct INSSBufferVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(INSSBuffer*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(INSSBuffer*);
    ULONG   (STDMETHODCALLTYPE *Release)(INSSBuffer*);
    HRESULT (STDMETHODCALLTYPE *GetLength)(INSSBuffer*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetLength)(INSSBuffer*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetMaxLength)(INSSBuffer*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(INSSBuffer*, BYTE**);
    HRESULT (STDMETHODCALLTYPE *GetBufferAndLength)(INSSBuffer*, BYTE**, DWORD*);
} INSSBufferVtbl;
struct INSSBuffer { const INSSBufferVtbl *lpVtbl; };

/* ========== IWMOutputMediaProps ========== */

typedef struct IWMOutputMediaProps IWMOutputMediaProps;
typedef struct IWMOutputMediaPropsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWMOutputMediaProps*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IWMOutputMediaProps*);
    ULONG   (STDMETHODCALLTYPE *Release)(IWMOutputMediaProps*);
    /* IWMMediaProps */
    HRESULT (STDMETHODCALLTYPE *GetType)(IWMOutputMediaProps*, GUID*);
    HRESULT (STDMETHODCALLTYPE *GetMediaType)(IWMOutputMediaProps*, WM_MEDIA_TYPE*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetMediaType)(IWMOutputMediaProps*, WM_MEDIA_TYPE*);
    /* IWMOutputMediaProps extension */
    HRESULT (STDMETHODCALLTYPE *GetStreamGroupName)(IWMOutputMediaProps*, WCHAR*, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetConnectionName)(IWMOutputMediaProps*, WCHAR*, WORD*);
} IWMOutputMediaPropsVtbl;
struct IWMOutputMediaProps { const IWMOutputMediaPropsVtbl *lpVtbl; };

/* ========== IWMStreamConfig ========== */

typedef struct IWMStreamConfig IWMStreamConfig;
typedef struct IWMStreamConfigVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWMStreamConfig*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IWMStreamConfig*);
    ULONG   (STDMETHODCALLTYPE *Release)(IWMStreamConfig*);
    HRESULT (STDMETHODCALLTYPE *GetStreamType)(IWMStreamConfig*, GUID*);
    HRESULT (STDMETHODCALLTYPE *GetStreamNumber)(IWMStreamConfig*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetStreamNumber)(IWMStreamConfig*, WORD);
    HRESULT (STDMETHODCALLTYPE *GetStreamName)(IWMStreamConfig*, WCHAR*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetStreamName)(IWMStreamConfig*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *GetConnectionName)(IWMStreamConfig*, WCHAR*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetConnectionName)(IWMStreamConfig*, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *GetBitrate)(IWMStreamConfig*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetBitrate)(IWMStreamConfig*, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetBufferWindow)(IWMStreamConfig*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetBufferWindow)(IWMStreamConfig*, DWORD);
} IWMStreamConfigVtbl;
struct IWMStreamConfig { const IWMStreamConfigVtbl *lpVtbl; };

/* ========== IWMProfile ========== */

typedef struct IWMProfile IWMProfile;
typedef struct IWMProfileVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWMProfile*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IWMProfile*);
    ULONG   (STDMETHODCALLTYPE *Release)(IWMProfile*);
    HRESULT (STDMETHODCALLTYPE *GetVersion)(IWMProfile*, WMT_VERSION*);
    HRESULT (STDMETHODCALLTYPE *GetName)(IWMProfile*, WCHAR*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetName)(IWMProfile*, const WCHAR*);
    HRESULT (STDMETHODCALLTYPE *GetDescription)(IWMProfile*, WCHAR*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *SetDescription)(IWMProfile*, const WCHAR*);
    HRESULT (STDMETHODCALLTYPE *GetStreamCount)(IWMProfile*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetStream)(IWMProfile*, DWORD, IWMStreamConfig**);
    HRESULT (STDMETHODCALLTYPE *GetStreamByNumber)(IWMProfile*, WORD, IWMStreamConfig**);
    HRESULT (STDMETHODCALLTYPE *RemoveStream)(IWMProfile*, IWMStreamConfig*);
    HRESULT (STDMETHODCALLTYPE *RemoveStreamByNumber)(IWMProfile*, WORD);
    HRESULT (STDMETHODCALLTYPE *AddStream)(IWMProfile*, IWMStreamConfig*);
    HRESULT (STDMETHODCALLTYPE *ReconfigStream)(IWMProfile*, IWMStreamConfig*);
    HRESULT (STDMETHODCALLTYPE *CreateNewStream)(IWMProfile*, REFGUID, IWMStreamConfig**);
    HRESULT (STDMETHODCALLTYPE *GetMutualExclusionCount)(IWMProfile*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetMutualExclusion)(IWMProfile*, DWORD, void**);
    HRESULT (STDMETHODCALLTYPE *RemoveMutualExclusion)(IWMProfile*, void*);
    HRESULT (STDMETHODCALLTYPE *AddMutualExclusion)(IWMProfile*, void*);
} IWMProfileVtbl;
struct IWMProfile { const IWMProfileVtbl *lpVtbl; };

/* ========== IWMSyncReader ========== */

typedef struct IWMSyncReader IWMSyncReader;
typedef struct IWMSyncReaderVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWMSyncReader*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IWMSyncReader*);
    ULONG   (STDMETHODCALLTYPE *Release)(IWMSyncReader*);
    HRESULT (STDMETHODCALLTYPE *Open)(IWMSyncReader*, const WCHAR*);
    HRESULT (STDMETHODCALLTYPE *Close)(IWMSyncReader*);
    HRESULT (STDMETHODCALLTYPE *SetRange)(IWMSyncReader*, QWORD, LONGLONG);
    HRESULT (STDMETHODCALLTYPE *SetRangeByFrame)(IWMSyncReader*, WORD, QWORD, LONGLONG);
    HRESULT (STDMETHODCALLTYPE *GetNextSample)(IWMSyncReader*, WORD, INSSBuffer**, QWORD*, QWORD*, DWORD*, DWORD*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetStreamsSelected)(IWMSyncReader*, WORD, WORD*, WMT_STREAM_SELECTION*);
    HRESULT (STDMETHODCALLTYPE *GetStreamSelected)(IWMSyncReader*, WORD, WMT_STREAM_SELECTION*);
    HRESULT (STDMETHODCALLTYPE *SetReadStreamSamples)(IWMSyncReader*, WORD, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetReadStreamSamples)(IWMSyncReader*, WORD, BOOL*);
    HRESULT (STDMETHODCALLTYPE *GetOutputSetting)(IWMSyncReader*, DWORD, const WCHAR*, WMT_ATTR_DATATYPE*, BYTE*, WORD*);
    HRESULT (STDMETHODCALLTYPE *SetOutputSetting)(IWMSyncReader*, DWORD, const WCHAR*, WMT_ATTR_DATATYPE, const BYTE*, WORD);
    HRESULT (STDMETHODCALLTYPE *GetOutputCount)(IWMSyncReader*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetOutputProps)(IWMSyncReader*, DWORD, IWMOutputMediaProps**);
    HRESULT (STDMETHODCALLTYPE *SetOutputProps)(IWMSyncReader*, DWORD, IWMOutputMediaProps*);
    HRESULT (STDMETHODCALLTYPE *GetOutputFormatCount)(IWMSyncReader*, DWORD, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetOutputFormat)(IWMSyncReader*, DWORD, DWORD, IWMOutputMediaProps**);
    HRESULT (STDMETHODCALLTYPE *GetOutputNumberForStream)(IWMSyncReader*, WORD, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetStreamNumberForOutput)(IWMSyncReader*, DWORD, WORD*);
    HRESULT (STDMETHODCALLTYPE *GetMaxOutputSampleSize)(IWMSyncReader*, DWORD, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetMaxStreamSampleSize)(IWMSyncReader*, WORD, DWORD*);
    HRESULT (STDMETHODCALLTYPE *OpenStream)(IWMSyncReader*, IStream*);
} IWMSyncReaderVtbl;
struct IWMSyncReader { const IWMSyncReaderVtbl *lpVtbl; };

/* ========== IWMProfileManager ========== */

typedef struct IWMProfileManager IWMProfileManager;
typedef struct IWMProfileManagerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IWMProfileManager*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IWMProfileManager*);
    ULONG   (STDMETHODCALLTYPE *Release)(IWMProfileManager*);
    HRESULT (STDMETHODCALLTYPE *CreateEmptyProfile)(IWMProfileManager*, WMT_VERSION, IWMProfile**);
    HRESULT (STDMETHODCALLTYPE *LoadProfileByID)(IWMProfileManager*, REFGUID, IWMProfile**);
    HRESULT (STDMETHODCALLTYPE *LoadProfileByData)(IWMProfileManager*, const WCHAR*, IWMProfile**);
    HRESULT (STDMETHODCALLTYPE *SaveProfile)(IWMProfileManager*, IWMProfile*, WCHAR*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *GetSystemProfileCount)(IWMProfileManager*, DWORD*);
    HRESULT (STDMETHODCALLTYPE *LoadSystemProfile)(IWMProfileManager*, DWORD, IWMProfile**);
} IWMProfileManagerVtbl;
struct IWMProfileManager { const IWMProfileManagerVtbl *lpVtbl; };

#endif /* WM_TYPES_H */
