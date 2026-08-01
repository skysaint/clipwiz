// imagecodec.h — 图片编解码，基于 Windows 自带的 WIC（不是第三方库）
// 剪贴板里的 DIB 统一转成 32 位 BGRA，落盘存 PNG；粘贴时再转回 DIB。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace imagecodec {

bool Init();
void Shutdown();

// 剪贴板 CF_DIB / CF_DIBV5 内存块 → PNG 字节流
bool DibToPng(const void* dib, size_t dibSize, std::vector<uint8_t>& png, uint32_t& width,
              uint32_t& height);

// 剪贴板 CF_BITMAP 兜底路径
bool HBitmapToPng(HBITMAP bitmap, std::vector<uint8_t>& png, uint32_t& width, uint32_t& height);

// PNG 字节流 → 写回剪贴板用的两份内存块（V5 带 alpha，普通 DIB 兼容老程序）
bool PngToDibs(const uint8_t* png, size_t size, std::vector<uint8_t>& dibV5,
               std::vector<uint8_t>& dib, uint32_t& width, uint32_t& height);

// 从 PNG 字节流生成预乘 alpha 的缩略图，供 AlphaBlend 直接画；调用方负责 DeleteObject
HBITMAP LoadThumbnailFromMemory(const uint8_t* png, size_t size, int maxW, int maxH, int& outW,
                                int& outH);

}  // namespace imagecodec
