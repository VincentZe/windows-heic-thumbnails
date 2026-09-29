#define NOMINMAX
#include "fast_thumbnail.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <libheif/heif.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint64_t kMaxPreviewBytes = 8 * 1024 * 1024;
constexpr UINT kMaxRawThumbnailSize = 512;

bool ReadAt(IStream* stream, uint64_t offset, void* output, size_t size, uint64_t fileSize)
{
    if (offset > fileSize || size > fileSize - offset || size > ULONG_MAX || offset > INT64_MAX)
        return false;
    LARGE_INTEGER position = {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    ULONG read = 0;
    return SUCCEEDED(stream->Seek(position, STREAM_SEEK_SET, nullptr)) &&
        SUCCEEDED(stream->Read(output, static_cast<ULONG>(size), &read)) && read == size;
}

uint16_t Number16(const uint8_t* data, bool little)
{
    return little ? static_cast<uint16_t>(data[0] | (data[1] << 8)) :
        static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t Number32(const uint8_t* data, bool little)
{
    return little ? static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24) :
        (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

bool TiffHeader(const uint8_t* data, size_t size, size_t offset, bool* little)
{
    if (offset > size || size - offset < 8) return false;
    *little = data[offset] == 'I' && data[offset + 1] == 'I';
    return (*little || (data[offset] == 'M' && data[offset + 1] == 'M')) &&
        Number16(data + offset + 2, *little) == 42;
}

uint32_t EntryValue(const uint8_t* entry, bool little)
{
    return Number16(entry + 2, little) == 3 ? Number16(entry + 8, little) : Number32(entry + 8, little);
}

bool FindExifJpeg(const std::vector<uint8_t>& exif, const uint8_t** jpeg, size_t* length,
    UINT* orientation)
{
    // HEIF Exif items may prefix the TIFF header with an offset and "Exif\0\0".
    for (size_t base = 0; base < std::min<size_t>(32, exif.size()); ++base)
    {
        bool little = false;
        if (!TiffHeader(exif.data(), exif.size(), base, &little)) continue;
        uint32_t ifd0 = Number32(exif.data() + base + 4, little);
        if (ifd0 > exif.size() - base || exif.size() - base - ifd0 < 2) return false;
        const size_t start = base + ifd0;
        const uint16_t count = Number16(exif.data() + start, little);
        if (count > 256 || static_cast<uint64_t>(start) + 2 + 12ull * count + 4 > exif.size()) return false;
        for (uint16_t i = 0; i < count; ++i)
        {
            const uint8_t* entry = exif.data() + start + 2 + 12 * i;
            if (Number16(entry, little) == 274 && Number16(entry + 2, little) == 3 &&
                Number32(entry + 4, little) == 1)
            {
                const uint16_t value = Number16(entry + 8, little);
                if (value >= 1 && value <= 8) *orientation = value;
            }
        }
        uint32_t ifd1 = Number32(exif.data() + start + 2 + 12 * count, little);
        if (!ifd1 || ifd1 > exif.size() - base || exif.size() - base - ifd1 < 2) return false;
        const size_t second = base + ifd1;
        const uint16_t entries = Number16(exif.data() + second, little);
        if (entries > 256 || static_cast<uint64_t>(second) + 2 + 12ull * entries > exif.size()) return false;
        uint32_t offset = 0, bytes = 0;
        for (uint16_t i = 0; i < entries; ++i)
        {
            const uint8_t* entry = exif.data() + second + 2 + 12 * i;
            if (Number32(entry + 4, little) != 1) continue;
            if (Number16(entry, little) == 0x201) offset = EntryValue(entry, little);
            if (Number16(entry, little) == 0x202) bytes = EntryValue(entry, little);
        }
        if (bytes < 4 || bytes > kMaxPreviewBytes || offset > exif.size() - base ||
            bytes > exif.size() - base - offset) return false;
        const uint8_t* candidate = exif.data() + base + offset;
        if (candidate[0] != 0xff || candidate[1] != 0xd8 ||
            candidate[bytes - 2] != 0xff || candidate[bytes - 1] != 0xd9) return false;
        *jpeg = candidate;
        *length = bytes;
        return true;
    }
    return false;
}

HRESULT BitmapFromPixels(const uint8_t* pixels, UINT width, UINT height, UINT orientation,
    HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    const bool quarterTurn = orientation >= 5 && orientation <= 8;
    const UINT outputWidth = quarterTurn ? height : width;
    const UINT outputHeight = quarterTurn ? width : height;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(outputWidth);
    info.bmiHeader.biHeight = -static_cast<LONG>(outputHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    uint8_t* destination = nullptr;
    HBITMAP result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&destination), nullptr, 0);
    if (!result) return HRESULT_FROM_WIN32(GetLastError());
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            UINT dx = x, dy = y;
            if (orientation == 2) dx = width - 1 - x;
            if (orientation == 3) { dx = width - 1 - x; dy = height - 1 - y; }
            if (orientation == 4) dy = height - 1 - y;
            if (orientation == 5) { dx = y; dy = x; }
            if (orientation == 6) { dx = height - 1 - y; dy = x; }
            if (orientation == 7) { dx = height - 1 - y; dy = width - 1 - x; }
            if (orientation == 8) { dx = y; dy = width - 1 - x; }
            const size_t from = (static_cast<size_t>(y) * width + x) * 4;
            const size_t to = (static_cast<size_t>(dy) * outputWidth + dx) * 4;
            std::copy_n(pixels + from, 4, destination + to);
        }
    *bitmap = result;
    *alpha = WTSAT_ARGB;
    return S_OK;
}

HRESULT DecodeJpeg(const uint8_t* jpeg, size_t bytes, UINT requested, UINT orientation,
    HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    if (bytes > ULONG_MAX) return E_INVALIDARG;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IStream> source;
    source.Attach(SHCreateMemStream(jpeg, static_cast<UINT>(bytes)));
    if (!source) return E_OUTOFMEMORY;
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(source.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;
    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || !width || !height) return E_FAIL;
    const UINT limit = requested ? requested : 256;
    const UINT longest = std::max(width, height);
    const UINT outWidth = longest > limit ? std::max(1u, static_cast<UINT>(uint64_t(width) * limit / longest)) : width;
    const UINT outHeight = longest > limit ? std::max(1u, static_cast<UINT>(uint64_t(height) * limit / longest)) : height;
    ComPtr<IWICBitmapSource> bitmapSource;
    if (outWidth != width || outHeight != height)
    {
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(), outWidth, outHeight,
            WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        bitmapSource = scaler;
    }
    else bitmapSource = frame;
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(bitmapSource.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    std::vector<uint8_t> pixels(static_cast<size_t>(outWidth) * outHeight * 4);
    hr = converter->CopyPixels(nullptr, outWidth * 4, static_cast<UINT>(pixels.size()), pixels.data());
    return FAILED(hr) ? hr : BitmapFromPixels(pixels.data(), outWidth, outHeight,
        orientation, bitmap, alpha);
}

struct HeifStream
{
    IStream* stream;
    uint64_t size;
};

int64_t HeifPosition(void* user)
{
    LARGE_INTEGER zero = {};
    ULARGE_INTEGER position = {};
    return SUCCEEDED(static_cast<HeifStream*>(user)->stream->Seek(zero, STREAM_SEEK_CUR, &position)) ?
        static_cast<int64_t>(position.QuadPart) : -1;
}

int HeifRead(void* output, size_t size, void* user)
{
    auto* reader = static_cast<HeifStream*>(user);
    if (size > ULONG_MAX) return 1;
    ULONG read = 0;
    return SUCCEEDED(reader->stream->Read(output, static_cast<ULONG>(size), &read)) && read == size ? 0 : 1;
}

int HeifSeek(int64_t position, void* user)
{
    if (position < 0) return 1;
    LARGE_INTEGER target = {};
    target.QuadPart = position;
    return SUCCEEDED(static_cast<HeifStream*>(user)->stream->Seek(target, STREAM_SEEK_SET, nullptr)) ? 0 : 1;
}

heif_reader_grow_status HeifWait(int64_t size, void* user)
{
    return size >= 0 && static_cast<uint64_t>(size) <= static_cast<HeifStream*>(user)->size ?
        heif_reader_grow_status_size_reached : heif_reader_grow_status_size_beyond_eof;
}

HRESULT HeifEmbeddedThumbnail(heif_image_handle* primary, UINT requested,
    HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    heif_item_id ids[16] = {};
    const int count = heif_image_handle_get_list_of_thumbnail_IDs(primary, ids, ARRAYSIZE(ids));
    if (count <= 0) return S_FALSE;
    heif_image_handle* thumbnail = nullptr;
    heif_error error = heif_image_handle_get_thumbnail(primary, ids[0], &thumbnail);
    if (error.code != heif_error_Ok || !thumbnail) return S_FALSE;
    const int sourceWidth = heif_image_handle_get_width(thumbnail);
    const int sourceHeight = heif_image_handle_get_height(thumbnail);
    HRESULT result = S_FALSE;
    if (sourceWidth > 0 && sourceHeight > 0 &&
        static_cast<uint64_t>(sourceWidth) * sourceHeight <= 2048ull * 2048)
    {
        heif_image* image = nullptr;
        error = heif_decode_image(thumbnail, &image, heif_colorspace_RGB,
            heif_chroma_interleaved_RGBA, nullptr);
        if (error.code == heif_error_Ok && image)
        {
            const UINT limit = requested ? requested : 256;
            const UINT longest = std::max(sourceWidth, sourceHeight);
            const UINT width = longest > limit ? std::max(1u,
                static_cast<UINT>(uint64_t(sourceWidth) * limit / longest)) : sourceWidth;
            const UINT height = longest > limit ? std::max(1u,
                static_cast<UINT>(uint64_t(sourceHeight) * limit / longest)) : sourceHeight;
            if (width != static_cast<UINT>(sourceWidth) || height != static_cast<UINT>(sourceHeight))
            {
                heif_image* scaled = nullptr;
                error = heif_image_scale_image(image, &scaled, width, height, nullptr);
                if (error.code == heif_error_Ok && scaled)
                {
                    heif_image_release(image);
                    image = scaled;
                }
            }
            if (error.code == heif_error_Ok)
            {
                int stride = 0;
                const uint8_t* rgba = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
                if (rgba && stride >= static_cast<int>(width * 4))
                {
                    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
                    for (UINT y = 0; y < height; ++y)
                        for (UINT x = 0; x < width; ++x)
                        {
                            const uint8_t* src = rgba + static_cast<size_t>(y) * stride + 4 * x;
                            uint8_t* dst = bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
                            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                        }
                    result = BitmapFromPixels(bgra.data(), width, height, 1, bitmap, alpha);
                }
            }
            heif_image_release(image);
        }
    }
    heif_image_handle_release(thumbnail);
    return result;
}

HRESULT HeicExifPreview(IStream* stream, uint64_t fileSize, UINT requested,
    HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    heif_context* context = heif_context_alloc();
    if (!context) return S_FALSE;
    HeifStream user = { stream, fileSize };
    const heif_reader reader = { 1, HeifPosition, HeifRead, HeifSeek, HeifWait };
    LARGE_INTEGER start = {};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
    {
        heif_context_free(context);
        return S_FALSE;
    }
    heif_error error = heif_context_read_from_reader(context, &reader, &user, nullptr);
    heif_image_handle* image = nullptr;
    if (error.code == heif_error_Ok) error = heif_context_get_primary_image_handle(context, &image);
    HRESULT result = S_FALSE;
    if (error.code == heif_error_Ok && image)
    {
        const int count = heif_image_handle_get_number_of_metadata_blocks(image, "Exif");
        heif_item_id ids[8] = {};
        const int found = heif_image_handle_get_list_of_metadata_block_IDs(image, "Exif", ids, 8);
        for (int i = 0; i < count && i < found && result != S_OK; ++i)
        {
            const size_t bytes = heif_image_handle_get_metadata_size(image, ids[i]);
            if (!bytes || bytes > kMaxPreviewBytes) continue;
            std::vector<uint8_t> exif(bytes);
            error = heif_image_handle_get_metadata(image, ids[i], exif.data());
            const uint8_t* jpeg = nullptr;
            size_t jpegSize = 0;
            UINT orientation = 1;
            if (error.code == heif_error_Ok && FindExifJpeg(exif, &jpeg, &jpegSize, &orientation))
                result = DecodeJpeg(jpeg, jpegSize, requested, orientation, bitmap, alpha);
        }
        if (result != S_OK)
            result = HeifEmbeddedThumbnail(image, requested, bitmap, alpha);
        heif_image_handle_release(image);
    }
    heif_context_free(context);
    return result;
}

struct TiffEntry { uint16_t type = 0; uint32_t count = 0; uint32_t value = 0; uint8_t inlineData[4] = {}; };

class TiffStream
{
public:
    TiffStream(IStream* source, uint64_t bytes, bool isLittle) : stream(source), size(bytes), little(isLittle) {}

    bool Directory(uint32_t offset, std::vector<std::pair<uint16_t, TiffEntry>>* entries,
        uint32_t* next) const
    {
        uint8_t countData[2];
        if (!ReadAt(stream, offset, countData, 2, size)) return false;
        const uint16_t count = Number16(countData, little);
        if (count > 256 || static_cast<uint64_t>(offset) + 2 + 12ull * count + 4 > size) return false;
        std::vector<uint8_t> data(12ull * count + 4);
        if (!ReadAt(stream, offset + 2, data.data(), data.size(), size)) return false;
        for (uint16_t i = 0; i < count; ++i)
        {
            const uint8_t* item = data.data() + i * 12;
            TiffEntry entry;
            entry.type = Number16(item + 2, little);
            entry.count = Number32(item + 4, little);
            entry.value = Number32(item + 8, little);
            std::copy_n(item + 8, 4, entry.inlineData);
            entries->emplace_back(Number16(item, little), entry);
        }
        *next = Number32(data.data() + count * 12, little);
        return true;
    }

    bool Values(const TiffEntry* entry, size_t maximum, std::vector<uint32_t>* values) const
    {
        if (!entry || !entry->count || entry->count > maximum ||
            (entry->type != 1 && entry->type != 3 && entry->type != 4)) return false;
        const size_t unit = entry->type == 1 ? 1 : entry->type == 3 ? 2 : 4;
        const size_t bytes = unit * entry->count;
        std::vector<uint8_t> data(bytes);
        if (bytes <= 4) std::copy_n(entry->inlineData, bytes, data.data());
        else if (!ReadAt(stream, entry->value, data.data(), bytes, size)) return false;
        for (uint32_t i = 0; i < entry->count; ++i)
            values->push_back(unit == 1 ? data[i] : unit == 2 ?
                Number16(data.data() + i * 2, little) : Number32(data.data() + i * 4, little));
        return true;
    }

    bool Rationals(const TiffEntry* entry, size_t count, std::vector<double>* result) const
    {
        if (!entry || entry->type != 5 || entry->count != count || count > 16) return false;
        std::vector<uint8_t> data(count * 8);
        if (!ReadAt(stream, entry->value, data.data(), data.size(), size)) return false;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t numerator = Number32(data.data() + i * 8, little);
            const uint32_t denominator = Number32(data.data() + i * 8 + 4, little);
            if (!denominator) return false;
            result->push_back(double(numerator) / denominator);
        }
        return true;
    }

    bool Read(uint64_t offset, void* output, size_t bytes) const { return ReadAt(stream, offset, output, bytes, size); }
    uint16_t Pixel(const uint8_t* data) const { return Number16(data, little); }
    uint64_t size;

private:
    IStream* stream;
    bool little;
};

const TiffEntry* Tag(const std::vector<std::pair<uint16_t, TiffEntry>>& entries, uint16_t wanted)
{
    for (const auto& item : entries) if (item.first == wanted) return &item.second;
    return nullptr;
}

uint32_t Scalar(const TiffStream& tiff, const std::vector<std::pair<uint16_t, TiffEntry>>& entries,
    uint16_t tag, uint32_t fallback = 0)
{
    std::vector<uint32_t> numbers;
    return tiff.Values(Tag(entries, tag), 1, &numbers) ? numbers[0] : fallback;
}

uint8_t Tonemap(uint16_t sample, double black, double white, double gain)
{
    const double value = std::clamp((sample - black) / (white - black) * gain, 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(std::pow(value, 1.0 / 2.2) * 255));
}

HRESULT SampleRaw(const TiffStream& tiff, const std::vector<std::pair<uint16_t, TiffEntry>>& entries,
    UINT requested, HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    const uint32_t width = Scalar(tiff, entries, 256), height = Scalar(tiff, entries, 257);
    const uint32_t rowsPerStrip = Scalar(tiff, entries, 278);
    if (width < 2 || height < 2 || width > 30000 || height > 30000 || !rowsPerStrip ||
        Scalar(tiff, entries, 258) != 16 || Scalar(tiff, entries, 259) != 1 ||
        Scalar(tiff, entries, 262) != 32803 || Scalar(tiff, entries, 277, 1) != 1) return E_NOTIMPL;
    const uint32_t orientation = Scalar(tiff, entries, 274, 1);
    if (orientation < 1 || orientation > 8) return E_NOTIMPL;
    std::vector<uint32_t> offsets, lengths, pattern, repeat;
    const size_t count = (height + static_cast<uint64_t>(rowsPerStrip) - 1) / rowsPerStrip;
    if (count > 30000 || !tiff.Values(Tag(entries, 273), count, &offsets) || offsets.size() != count ||
        !tiff.Values(Tag(entries, 279), count, &lengths) || lengths.size() != count ||
        !tiff.Values(Tag(entries, 33421), 2, &repeat) || repeat != std::vector<uint32_t>({ 2, 2 }) ||
        !tiff.Values(Tag(entries, 33422), 4, &pattern) || pattern.size() != 4) return E_NOTIMPL;
    for (uint32_t color : pattern) if (color > 2) return E_NOTIMPL;
    const double white = Scalar(tiff, entries, 50717, 65535);
    std::vector<double> neutral, blackLevels;
    tiff.Rationals(Tag(entries, 50728), 3, &neutral);
    const TiffEntry* blackTag = Tag(entries, 50714);
    if (blackTag) tiff.Rationals(blackTag, blackTag->count, &blackLevels);
    const double black = blackLevels.empty() ? 0 : blackLevels.front();
    if (white <= black) return E_NOTIMPL;
    double gain[3] = { 1, 1, 1 };
    if (neutral.size() == 3 && neutral[0] > 0 && neutral[2] > 0)
    {
        gain[0] = std::clamp(neutral[1] / neutral[0], 0.25, 4.0);
        gain[2] = std::clamp(neutral[1] / neutral[2], 0.25, 4.0);
    }
    const UINT limit = std::clamp(requested, 1u, kMaxRawThumbnailSize);
    const uint32_t longest = std::max(width, height);
    const UINT outWidth = std::max(1u, static_cast<UINT>(uint64_t(width) * limit / longest));
    const UINT outHeight = std::max(1u, static_cast<UINT>(uint64_t(height) * limit / longest));
    std::vector<uint8_t> pixels(static_cast<size_t>(outWidth) * outHeight * 4);
    std::vector<uint8_t> row0(width * 2), row1(width * 2);
    auto readRow = [&](uint32_t row, std::vector<uint8_t>& target) {
        const size_t strip = row / rowsPerStrip;
        const uint64_t inStrip = uint64_t(row % rowsPerStrip) * width * 2;
        return inStrip <= lengths[strip] && target.size() <= lengths[strip] - inStrip &&
            tiff.Read(uint64_t(offsets[strip]) + inStrip, target.data(), target.size());
    };
    for (UINT y = 0; y < outHeight; ++y)
    {
        uint32_t sy = static_cast<uint32_t>(uint64_t(y) * height / outHeight) & ~1u;
        sy = std::min(sy, (height - 2) & ~1u);
        if (!readRow(sy, row0) || !readRow(sy + 1, row1)) return E_FAIL;
        for (UINT x = 0; x < outWidth; ++x)
        {
            uint32_t sx = static_cast<uint32_t>(uint64_t(x) * width / outWidth) & ~1u;
            sx = std::min(sx, (width - 2) & ~1u);
            const uint16_t samples[4] = { tiff.Pixel(row0.data() + sx * 2),
                tiff.Pixel(row0.data() + (sx + 1) * 2), tiff.Pixel(row1.data() + sx * 2),
                tiff.Pixel(row1.data() + (sx + 1) * 2) };
            uint32_t sums[3] = {}, counts[3] = {};
            for (int i = 0; i < 4; ++i) { sums[pattern[i]] += samples[i]; ++counts[pattern[i]]; }
            const size_t dest = (static_cast<size_t>(y) * outWidth + x) * 4;
            for (int channel = 0; channel < 3; ++channel)
                pixels[dest + 2 - channel] = counts[channel] ? Tonemap(static_cast<uint16_t>(sums[channel] / counts[channel]),
                    black, white, gain[channel]) : 0;
            pixels[dest + 3] = 255;
        }
    }
    return BitmapFromPixels(pixels.data(), outWidth, outHeight, orientation, bitmap, alpha);
}

HRESULT DngPreview(IStream* stream, uint64_t fileSize, const uint8_t* header, UINT requested,
    HBITMAP* bitmap, WTS_ALPHATYPE* alpha)
{
    bool little = false;
    if (!TiffHeader(header, 8, 0, &little)) return E_NOTIMPL;
    TiffStream tiff(stream, fileSize, little);
    uint32_t offset = Number32(header + 4, little);
    std::vector<std::pair<uint16_t, TiffEntry>> raw;
    std::vector<uint32_t> pending = { offset };
    for (size_t visited = 0; visited < pending.size() && visited < 12; ++visited)
    {
        offset = pending[visited];
        if (!offset) continue;
        std::vector<std::pair<uint16_t, TiffEntry>> entries;
        uint32_t next = 0;
        if (!tiff.Directory(offset, &entries, &next)) continue;
        if (visited == 0) raw = entries;
        if (next && std::find(pending.begin(), pending.end(), next) == pending.end()) pending.push_back(next);
        std::vector<uint32_t> sub;
        if (tiff.Values(Tag(entries, 330), 8, &sub))
            for (uint32_t child : sub)
                if (child && std::find(pending.begin(), pending.end(), child) == pending.end()) pending.push_back(child);
        uint32_t jpegOffset = Scalar(tiff, entries, 513), jpegSize = Scalar(tiff, entries, 514);
        if ((!jpegOffset || !jpegSize) && Scalar(tiff, entries, 259) == 7)
        {
            jpegOffset = Scalar(tiff, entries, 273);
            jpegSize = Scalar(tiff, entries, 279);
        }
        if (jpegSize > 4 && jpegSize <= kMaxPreviewBytes)
        {
            std::vector<uint8_t> jpeg(jpegSize);
            if (tiff.Read(jpegOffset, jpeg.data(), jpeg.size()) && jpeg[0] == 0xff && jpeg[1] == 0xd8)
            {
                HRESULT hr = DecodeJpeg(jpeg.data(), jpeg.size(), requested,
                    Scalar(tiff, entries, 274, 1), bitmap, alpha);
                if (SUCCEEDED(hr)) return hr;
            }
        }
    }
    return SampleRaw(tiff, raw, requested, bitmap, alpha);
}

} // namespace

HRESULT CreateFastThumbnail(IStream* stream, UINT requestedSize, HBITMAP* bitmap,
    WTS_ALPHATYPE* alpha, bool* isDng)
{
    *isDng = false;
    ULARGE_INTEGER length = {};
    HRESULT hr = IStream_Size(stream, &length);
    if (FAILED(hr)) return hr;
    uint8_t header[12] = {};
    if (!ReadAt(stream, 0, header, sizeof(header), length.QuadPart)) return E_FAIL;
    if (TiffHeader(header, sizeof(header), 0, isDng))
        return DngPreview(stream, length.QuadPart, header, requestedSize, bitmap, alpha);
    *isDng = false;
    if (header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p')
        return HeicExifPreview(stream, length.QuadPart, requestedSize, bitmap, alpha);
    return S_FALSE;
}
