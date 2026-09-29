#pragma once

#include <objidl.h>
#include <thumbcache.h>

// S_OK: thumbnail produced; S_FALSE: HEIC has no usable preview; failure: unsupported DNG.
HRESULT CreateFastThumbnail(IStream* stream, UINT requestedSize, HBITMAP* bitmap,
    WTS_ALPHATYPE* alpha, bool* isDng);
