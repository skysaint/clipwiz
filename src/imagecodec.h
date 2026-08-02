// imagecodec.h — Image encoding/decoding via Windows built-in WIC (no third-party library)
// Clipboard DIBs are unified to 32-bit BGRA, stored as PNG on disk; converted back to DIB for paste.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace imagecodec {

bool Init();  // Returns true on success, false on failure
void Shutdown();

// Clipboard CF_DIB / CF_DIBV5 memory block -> PNG byte stream
bool DibToPng(const void* dib, size_t dibSize, std::vector<uint8_t>& png, uint32_t& width,
              uint32_t& height);

// Clipboard CF_BITMAP fallback path
bool HBitmapToPng(HBITMAP bitmap, std::vector<uint8_t>& png, uint32_t& width, uint32_t& height);

// PNG byte stream -> two memory blocks for clipboard write (V5 with alpha, plain DIB for legacy)
bool PngToDibs(const uint8_t* png, size_t size, std::vector<uint8_t>& dibV5,
               std::vector<uint8_t>& dib, uint32_t& width, uint32_t& height);

// Generate premultiplied-alpha thumbnail from PNG for AlphaBlend; caller owns DeleteObject
HBITMAP LoadThumbnailFromMemory(const uint8_t* png, size_t size, int maxW, int maxH, int& outW,
                                int& outH);

}  // namespace imagecodec
