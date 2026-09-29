#include <shlwapi.h>
#include <thumbcache.h> // For IThumbnailProvider.
#include <new>

#include <libheif/heif.h>

#include "log.h"
#include "fast_thumbnail.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "Pathcch.lib")

// this thumbnail provider implements IInitializeWithStream to enable being hosted
// in an isolated process for robustness

class CHEICThumbProvider : public IInitializeWithStream,
    public IThumbnailProvider
{
public:
    CHEICThumbProvider() : _cRef(1), _pStream(NULL)
    {
    }

    virtual ~CHEICThumbProvider()
    {
        if (_pStream)
        {
            _pStream->Release();
        }

    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CHEICThumbProvider, IInitializeWithStream),
            QITABENT(CHEICThumbProvider, IThumbnailProvider),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        ULONG cRef = InterlockedDecrement(&_cRef);
        if (!cRef)
        {
            delete this;
        }
        return cRef;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* pStream, DWORD grfMode);

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha);

private:

    long _cRef;
    IStream* _pStream;     // provided during initialization.
};

HRESULT CHEICThumbProvider_CreateInstance(REFIID riid, void** ppv)
{
    CHEICThumbProvider* pNew = new (std::nothrow) CHEICThumbProvider();
    HRESULT hr = pNew ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pNew->QueryInterface(riid, ppv);
        pNew->Release();
    }
    return hr;
}

// IInitializeWithStream
IFACEMETHODIMP CHEICThumbProvider::Initialize(IStream* pStream, DWORD)
{
    HRESULT hr = E_UNEXPECTED;  // can only be inited once
    if (_pStream == NULL)
    {
        // take a reference to the stream if we have not been inited yet
        hr = pStream->QueryInterface(&_pStream);
    }
    return hr;
}

HRESULT CreateDIBFromData(HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha, const uint8_t* src_data, int thumbnail_width, int thumbnail_height, int src_stride)
{
    HRESULT hr = E_FAIL;

    UINT nWidth = thumbnail_width;
    UINT nHeight = thumbnail_height;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = nWidth;
    bmi.bmiHeader.biHeight = -static_cast<LONG>(nHeight);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    UINT dest_stride = ((((bmi.bmiHeader.biWidth * bmi.bmiHeader.biBitCount) + 31) & ~31) >> 3);

    BYTE* dest_data = 0;
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&dest_data), NULL, 0);
    hr = hbmp ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        for (UINT y = 0; y < nHeight; ++y)
        {
            // 0xAARRGGBB
            DWORD* dest_row = reinterpret_cast<DWORD*>(&dest_data[y * dest_stride]);

            // 0xAABBGGRR
            const DWORD* src_row = reinterpret_cast<const DWORD*>(&src_data[y * src_stride]);

            for (UINT x = 0; x < nWidth; ++x)
            {
                dest_row[x] = 
                    (src_row[x] & 0xFF000000) | 
                    ((src_row[x] & 0x00FF0000) >> 16) |
                    (src_row[x] & 0x0000FF00) | 
                    ((src_row[x] & 0x000000FF) << 16);
            }
        }
        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
    }
    else
    {
        Log_WriteFmt(LOG_ERROR, L"CreateDIBSection (%u x %u) failed: 0x%08x", nWidth, nHeight, GetLastError());
    }

    return hr;

}
// IThumbnailProvider
IFACEMETHODIMP CHEICThumbProvider::GetThumbnail(UINT requested_size, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    if (!phbmp || !pdwAlpha || !_pStream) return E_INVALIDARG;
    *phbmp = nullptr;
    bool isDng = false;
    HRESULT fastResult = CreateFastThumbnail(_pStream, requested_size, phbmp, pdwAlpha, &isDng);
    if (fastResult == S_OK) return S_OK;
    if (isDng) return fastResult == S_FALSE ? E_FAIL : fastResult;

    Log_WriteFmt(LOG_INFO, L"BEGIN Requested thumbnail size: %u x %u", requested_size, requested_size);

    ULARGE_INTEGER ulSize;

    HRESULT final_hr = E_FAIL;

    DWORD tick_start = GetTickCount();

    HRESULT hr = IStream_Size(_pStream, &ulSize);
    if (FAILED(hr))
    {
        Log_WriteFmt(LOG_ERROR, L"Failed to get image stream size, IStream_Size failed: 0x%08x", hr);
    }
    else
    {
        Log_WriteFmt(LOG_DEBUG, L"Stream size {%u, %u}", ulSize.HighPart, ulSize.LowPart);

        // A HEIC without a preview should not force an unbounded decode in Explorer.
        if (ulSize.HighPart == 0 && ulSize.LowPart <= 64 * 1024 * 1024)
        {
            LARGE_INTEGER start = {};
            hr = _pStream->Seek(start, STREAM_SEEK_SET, nullptr);
            if (FAILED(hr)) return hr;
            void* ptr = LocalAlloc(LPTR, ulSize.LowPart);
            if (!ptr)
            {
                Log_WriteFmt(LOG_ERROR, L"Could not allocate memory for image, LocalAlloc(%u) failed: 0x%08x", ulSize.LowPart, GetLastError());
            }
            else
            {
                ULONG ulRead = 0;
                hr = _pStream->Read(ptr, ulSize.LowPart, &ulRead);
                if (FAILED(hr) || ulRead != ulSize.LowPart)
                {
                    Log_WriteFmt(LOG_ERROR, L"Could not read image stream, IStream::Read(%u) failed: 0x%08x", ulSize.LowPart, hr);
                }
                else
                {
                    DWORD tick_read_end = GetTickCount();
                    Log_WriteFmt(LOG_TRACE, L"Stream read time: %u ms", tick_read_end - tick_start);

                    int input_width = 0;
                    int input_height = 0;

                    Log_WriteFmt(LOG_DEBUG, L"Stream read: %u", ulRead);

                    heif_context* ctx = heif_context_alloc();
                    heif_context_read_from_memory_without_copy(ctx, ptr, ulSize.LowPart, nullptr);

                    // --- get primary image
                    struct heif_image_handle* image_handle = NULL;
                    heif_error err = heif_context_get_primary_image_handle(ctx, &image_handle);
                    if (err.code) 
                    {
                        Log_WriteFmt(LOG_ERROR, L"Could not read HEIF image, heif_context_get_primary_image_handle failed: %S", err.message);
                    }
                    else
                    {
                        input_width = heif_image_handle_get_width(image_handle);
                        input_height = heif_image_handle_get_height(image_handle);

                        Log_WriteFmt(LOG_INFO, L"Full HEIF image size: %i x %i", input_width, input_height);

                        heif_item_id thumbnail_ID;
                        int nThumbnails = heif_image_handle_get_list_of_thumbnail_IDs(image_handle, &thumbnail_ID, 1);
                        if (nThumbnails > 0)
                        {
                            Log_WriteFmt(LOG_INFO, L"File contains %i thumbnail, loading thumbnail.", nThumbnails);

                            struct heif_image_handle* thumbnail_handle;
                            err = heif_image_handle_get_thumbnail(image_handle, thumbnail_ID, &thumbnail_handle);
                            if (err.code) 
                            {
                                Log_WriteFmt(LOG_ERROR, L"Could not load HEIF thumbnail, heif_image_handle_get_thumbnail failed: %S", err.message);
                            }
                            else
                            {
                                // replace image handle with thumbnail handle
                                heif_image_handle_release(image_handle);
                                image_handle = thumbnail_handle;

                                input_width = heif_image_handle_get_width(image_handle);
                                input_height = heif_image_handle_get_height(image_handle);

                                Log_WriteFmt(LOG_INFO, L"HEIF thumbnail size: %i x %i", input_width, input_height);
                            }
                        }
                        else
                        {
                            Log_Write(LOG_INFO, L"File does not contain thumbnail, decoding full image.");
                            if (static_cast<uint64_t>(input_width) * input_height > 32ull * 1024 * 1024)
                            {
                                heif_image_handle_release(image_handle);
                                image_handle = nullptr;
                                Log_Write(LOG_WARNING, L"Skipping oversized HEIC without embedded preview.");
                            }
                        }
                    }

                    if (image_handle)
                    {
                        struct heif_decoding_options* decode_options = heif_decoding_options_alloc();
                        decode_options->convert_hdr_to_8bit = true;

                        int bit_depth = 8;

                        DWORD tick_decode_start = GetTickCount();
                        Log_WriteFmt(LOG_TRACE, L"Decode setup time: %u ms", tick_decode_start - tick_read_end);

                        struct heif_image* image = NULL;
                        err = heif_decode_image(image_handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, decode_options);
                        if (err.code) 
                        {
                            Log_WriteFmt(LOG_ERROR, L"Could not decode HEIF image, heif_decode_image failed: %S", err.message);
                        }
                        else
                        {
                            DWORD tick_decode_end = GetTickCount();
                            Log_WriteFmt(LOG_INFO, L"HEIF decode time: %u ms", tick_decode_end - tick_decode_start);

                            int output_width = input_width;
                            int output_height = input_height;

                            bool scale_error = false;
                            if (input_width > (int)requested_size || input_height > (int)requested_size)
                            {
                                int scaled_w = input_width;
                                int scaled_h = input_height;

                                if (input_width > input_height)
                                {
                                    scaled_h = input_height * requested_size / input_width;
                                    scaled_w = requested_size;
                                }
                                else // (input_width <= input_height)
                                {
                                    scaled_w = input_width * requested_size / input_height;
                                    scaled_h = requested_size;
                                }

                                Log_WriteFmt(LOG_INFO, L"Scaling image %i x %i to %i x %i", input_width, input_height, scaled_w, scaled_h);

                                struct heif_image* scaled_image = NULL;
                                err = heif_image_scale_image(image, &scaled_image, scaled_w, scaled_h, NULL);
                                if (err.code) 
                                {
                                    Log_WriteFmt(LOG_ERROR, L"Could not scale image, heif_image_scale_image failed: %S", err.message);
                                    scale_error = true;
                                }
                                else
                                {
                                    heif_image_release(image);
                                    image = scaled_image;

                                    output_width = scaled_w;
                                    output_height = scaled_h;

                                    DWORD tick_scale_end = GetTickCount();
                                    Log_WriteFmt(LOG_TRACE, L"Image scale time: %u ms", tick_scale_end - tick_decode_end);
                                }
                            }

                            if (!scale_error)
                            {
                                int stride;
                                const uint8_t* data = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
                                if (data == NULL)
                                {
                                    Log_Write(LOG_ERROR, L"Could not get image data, heif_image_get_plane_readonly returned null");
                                    scale_error = true;
                                }
                                else
                                {
                                    DWORD tick_dib_start = GetTickCount();
                                    final_hr = CreateDIBFromData(phbmp, pdwAlpha, data, output_width, output_height, stride);

                                    DWORD tick_dib_end = GetTickCount();
                                    Log_WriteFmt(LOG_TRACE, L"Create DIB from HEIF time: %u ms", tick_dib_end - tick_dib_start);
                                }
                            }

                            heif_image_release(image);
                        }

                        heif_image_handle_release(image_handle);
                    }

                    heif_context_free(ctx);
                }

                LocalFree(ptr);
            }
        }
        else
        {
            Log_WriteFmt(LOG_WARNING, L"Skipping unexpectedly large image {%u, %u}", ulSize.HighPart, ulSize.LowPart);
        }
    }

    DWORD tick_end = GetTickCount();
    Log_WriteFmt(LOG_INFO, L"END Total time: %u ms", tick_end - tick_start);

    if (FAILED(hr))
    {
        return hr;
    }

    return final_hr;
}
