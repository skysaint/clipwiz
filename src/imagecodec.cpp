// imagecodec.cpp
#include "imagecodec.h"

#include <wincodec.h>

#include "util.h"

namespace imagecodec {
namespace {

// 极简 COM 智能指针，避免到处写 Release
template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { Reset(); }

    T** Put() { return &ptr_; }
    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

IWICImagingFactory* g_factory = nullptr;

struct DibInfo {
    const BITMAPINFOHEADER* head = nullptr;
    const uint8_t* bits = nullptr;
    size_t bitsSize = 0;
    int width = 0;
    int height = 0;   // 绝对值
    int bpp = 0;
    bool topDown = false;
};

size_t ColorTableBytes(const BITMAPINFOHEADER* head) {
    if (head->biBitCount <= 8) {
        DWORD used = head->biClrUsed ? head->biClrUsed : (1u << head->biBitCount);
        return static_cast<size_t>(used) * sizeof(RGBQUAD);
    }
    // 只有 BITMAPINFOHEADER 且 BI_BITFIELDS 时，头后面跟着三个掩码
    if (head->biCompression == BI_BITFIELDS && head->biSize == sizeof(BITMAPINFOHEADER)) {
        return 3 * sizeof(DWORD);
    }
    return 0;
}

bool ParseDib(const void* data, size_t size, DibInfo& out) {
    if (!data || size < sizeof(BITMAPINFOHEADER)) {
        return false;
    }
    const BITMAPINFOHEADER* head = static_cast<const BITMAPINFOHEADER*>(data);
    if (head->biSize < sizeof(BITMAPINFOHEADER) || head->biSize > size) {
        return false;
    }
    if (head->biCompression != BI_RGB && head->biCompression != BI_BITFIELDS) {
        return false;  // RLE 压缩的 DIB 极罕见，直接不收
    }
    if (head->biWidth <= 0 || head->biHeight == 0 || head->biPlanes != 1) {
        return false;
    }
    size_t offset = head->biSize + ColorTableBytes(head);
    if (offset >= size) {
        return false;
    }
    out.head = head;
    out.bits = static_cast<const uint8_t*>(data) + offset;
    out.bitsSize = size - offset;
    out.width = head->biWidth;
    out.topDown = head->biHeight < 0;
    out.height = out.topDown ? -head->biHeight : head->biHeight;
    out.bpp = head->biBitCount;
    return true;
}

// 任意格式 DIB → 自顶向下的 32 位 BGRA
bool DibToBgra(const void* data, size_t size, std::vector<uint8_t>& bgra, int& width,
               int& height) {
    DibInfo info;
    if (!ParseDib(data, size, info)) {
        return false;
    }
    width = info.width;
    height = info.height;
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    bgra.assign(rowBytes * static_cast<size_t>(height), 0);

    if (info.bpp == 32) {
        // 32 位直接搬，顺手保住 alpha
        const size_t srcStride = rowBytes;
        if (info.bitsSize < srcStride * static_cast<size_t>(height)) {
            return false;
        }
        bool anyAlpha = false;
        for (int y = 0; y < height; ++y) {
            int srcRow = info.topDown ? y : (height - 1 - y);
            const uint8_t* src = info.bits + static_cast<size_t>(srcRow) * srcStride;
            uint8_t* dst = bgra.data() + static_cast<size_t>(y) * rowBytes;
            memcpy(dst, src, rowBytes);
            for (size_t x = 3; x < rowBytes; x += 4) {
                if (dst[x] != 0) {
                    anyAlpha = true;
                }
            }
        }
        if (!anyAlpha) {
            // 有些程序放的 32 位图 alpha 全 0，那就是不透明
            for (size_t x = 3; x < bgra.size(); x += 4) {
                bgra[x] = 255;
            }
        }
        return true;
    }

    // 其他位深交给 GDI 转换，省掉手写调色板/掩码解码
    BITMAPINFO dstInfo{};
    dstInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dstInfo.bmiHeader.biWidth = width;
    dstInfo.bmiHeader.biHeight = -height;  // 自顶向下
    dstInfo.bmiHeader.biPlanes = 1;
    dstInfo.bmiHeader.biBitCount = 32;
    dstInfo.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    void* dstBits = nullptr;
    HBITMAP dstBmp = CreateDIBSection(screen, &dstInfo, DIB_RGB_COLORS, &dstBits, nullptr, 0);
    bool ok = false;
    if (dstBmp && dstBits) {
        HDC mem = CreateCompatibleDC(screen);
        if (mem) {
            HGDIOBJ old = SelectObject(mem, dstBmp);
            int lines = StretchDIBits(mem, 0, 0, width, height, 0, 0, width, height, info.bits,
                                      reinterpret_cast<const BITMAPINFO*>(info.head),
                                      DIB_RGB_COLORS, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
            if (lines != 0) {
                memcpy(bgra.data(), dstBits, bgra.size());
                for (size_t x = 3; x < bgra.size(); x += 4) {
                    bgra[x] = 255;
                }
                ok = true;
            }
        }
    }
    if (dstBmp) {
        DeleteObject(dstBmp);
    }
    ReleaseDC(nullptr, screen);
    return ok;
}

bool EncodePng(const uint8_t* bgra, int width, int height, std::vector<uint8_t>& png) {
    if (!g_factory || !bgra || width <= 0 || height <= 0) {
        return false;
    }
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put()))) {
        return false;
    }
    Com<IWICBitmapEncoder> encoder;
    if (FAILED(g_factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))) {
        return false;
    }
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.Put(), props.Put()))) {
        return false;
    }
    if (FAILED(frame->Initialize(props.Get()))) {
        return false;
    }
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) {
        return false;
    }
    const UINT stride = static_cast<UINT>(width) * 4;
    if (FAILED(frame->WritePixels(static_cast<UINT>(height), stride, stride * height,
                                  const_cast<BYTE*>(bgra)))) {
        return false;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }

    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global)) || !global) {
        return false;
    }
    SIZE_T size = GlobalSize(global);
    void* mapped = GlobalLock(global);
    if (!mapped || size == 0) {
        if (mapped) {
            GlobalUnlock(global);
        }
        return false;
    }
    png.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + size);
    GlobalUnlock(global);
    return true;
}

bool DecodePngMemory(const uint8_t* png, size_t size, std::vector<uint8_t>& bgra, uint32_t& width,
                     uint32_t& height, bool premultiplied, int maxW, int maxH) {
    if (!g_factory || !png || size == 0) {
        return false;
    }
    // 把 PNG 字节放进 IStream
    HGLOBAL hglobal = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hglobal) {
        return false;
    }
    void* locked = GlobalLock(hglobal);
    if (!locked) {
        GlobalFree(hglobal);
        return false;
    }
    memcpy(locked, png, size);
    GlobalUnlock(hglobal);

    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hglobal, TRUE, stream.Put()))) {
        return false;
    }
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(g_factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                  WICDecodeMetadataCacheOnDemand,
                                                  decoder.Put()))) {
        return false;
    }
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put()))) {
        return false;
    }
    UINT srcW = 0;
    UINT srcH = 0;
    if (FAILED(frame->GetSize(&srcW, &srcH)) || srcW == 0 || srcH == 0) {
        return false;
    }

    UINT dstW = srcW;
    UINT dstH = srcH;
    if (maxW > 0 && maxH > 0) {
        double scale = 1.0;
        double sx = static_cast<double>(maxW) / static_cast<double>(srcW);
        double sy = static_cast<double>(maxH) / static_cast<double>(srcH);
        scale = sx < sy ? sx : sy;
        if (scale > 1.0) {
            scale = 1.0;
        }
        dstW = static_cast<UINT>(srcW * scale);
        dstH = static_cast<UINT>(srcH * scale);
        if (dstW == 0) {
            dstW = 1;
        }
        if (dstH == 0) {
            dstH = 1;
        }
    }

    IWICBitmapSource* source = frame.Get();
    Com<IWICBitmapScaler> scaler;
    if (dstW != srcW || dstH != srcH) {
        if (FAILED(g_factory->CreateBitmapScaler(scaler.Put()))) {
            return false;
        }
        if (FAILED(scaler->Initialize(frame.Get(), dstW, dstH, WICBitmapInterpolationModeFant))) {
            return false;
        }
        source = scaler.Get();
    }

    Com<IWICBitmapSource> converted;
    const WICPixelFormatGUID target =
        premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(WICConvertBitmapSource(target, source, converted.Put()))) {
        return false;
    }

    const UINT stride = dstW * 4;
    bgra.assign(static_cast<size_t>(stride) * dstH, 0);
    if (FAILED(converted->CopyPixels(nullptr, stride, static_cast<UINT>(bgra.size()),
                                     bgra.data()))) {
        return false;
    }
    width = dstW;
    height = dstH;
    return true;
}

void FillBitmapInfoHeader(BITMAPINFOHEADER& head, uint32_t width, uint32_t height,
                          uint32_t imageBytes) {
    head.biSize = sizeof(BITMAPINFOHEADER);
    head.biWidth = static_cast<LONG>(width);
    head.biHeight = static_cast<LONG>(height);  // 正数 = 自底向上，兼容性最好
    head.biPlanes = 1;
    head.biBitCount = 32;
    head.biCompression = BI_RGB;
    head.biSizeImage = imageBytes;
}

// 把自顶向下的 BGRA 翻成自底向上
void FlipRowsInto(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height) {
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * rowBytes,
               src + static_cast<size_t>(height - 1 - y) * rowBytes, rowBytes);
    }
}

}  // namespace

bool Init() {
    if (g_factory) {
        return true;
    }
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_factory));
    return SUCCEEDED(hr) && g_factory != nullptr;
}

void Shutdown() {
    if (g_factory) {
        g_factory->Release();
        g_factory = nullptr;
    }
}

bool DibToPng(const void* dib, size_t dibSize, std::vector<uint8_t>& png, uint32_t& width,
              uint32_t& height) {
    std::vector<uint8_t> bgra;
    int w = 0;
    int h = 0;
    if (!DibToBgra(dib, dibSize, bgra, w, h)) {
        return false;
    }
    if (!EncodePng(bgra.data(), w, h, png)) {
        return false;
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}

bool HBitmapToPng(HBITMAP bitmap, std::vector<uint8_t>& png, uint32_t& width, uint32_t& height) {
    BITMAP info{};
    if (!bitmap || GetObjectW(bitmap, sizeof(info), &info) == 0) {
        return false;
    }
    if (info.bmWidth <= 0 || info.bmHeight <= 0) {
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra(static_cast<size_t>(info.bmWidth) * info.bmHeight * 4, 0);
    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    int lines = GetDIBits(screen, bitmap, 0, static_cast<UINT>(info.bmHeight), bgra.data(), &bi,
                          DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (lines == 0) {
        return false;
    }
    for (size_t x = 3; x < bgra.size(); x += 4) {
        bgra[x] = 255;  // HBITMAP 路径没有可靠的 alpha，当不透明处理
    }
    if (!EncodePng(bgra.data(), info.bmWidth, info.bmHeight, png)) {
        return false;
    }
    width = static_cast<uint32_t>(info.bmWidth);
    height = static_cast<uint32_t>(info.bmHeight);
    return true;
}

bool PngToDibs(const uint8_t* png, size_t size, std::vector<uint8_t>& dibV5,
               std::vector<uint8_t>& dib, uint32_t& width, uint32_t& height) {
    std::vector<uint8_t> bgra;
    if (!DecodePngMemory(png, size, bgra, width, height, false, 0, 0)) {
        return false;
    }
    const size_t imageBytes = bgra.size();

    dibV5.assign(sizeof(BITMAPV5HEADER) + imageBytes, 0);
    BITMAPV5HEADER* v5 = reinterpret_cast<BITMAPV5HEADER*>(dibV5.data());
    v5->bV5Size = sizeof(BITMAPV5HEADER);
    v5->bV5Width = static_cast<LONG>(width);
    v5->bV5Height = static_cast<LONG>(height);
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = static_cast<DWORD>(imageBytes);
    v5->bV5RedMask = 0x00FF0000;
    v5->bV5GreenMask = 0x0000FF00;
    v5->bV5BlueMask = 0x000000FF;
    v5->bV5AlphaMask = 0xFF000000;
    v5->bV5CSType = LCS_WINDOWS_COLOR_SPACE;
    v5->bV5Intent = LCS_GM_IMAGES;
    FlipRowsInto(dibV5.data() + sizeof(BITMAPV5HEADER), bgra.data(), width, height);

    dib.assign(sizeof(BITMAPINFOHEADER) + imageBytes, 0);
    FillBitmapInfoHeader(*reinterpret_cast<BITMAPINFOHEADER*>(dib.data()), width, height,
                         static_cast<uint32_t>(imageBytes));
    FlipRowsInto(dib.data() + sizeof(BITMAPINFOHEADER), bgra.data(), width, height);
    return true;
}

HBITMAP LoadThumbnailFromMemory(const uint8_t* png, size_t size, int maxW, int maxH, int& outW,
                                int& outH) {
    outW = 0;
    outH = 0;
    if (maxW <= 0 || maxH <= 0) {
        return nullptr;
    }
    std::vector<uint8_t> pbgra;
    uint32_t w = 0;
    uint32_t h = 0;
    if (!DecodePngMemory(png, size, pbgra, w, h, true, maxW, maxH)) {
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return nullptr;
    }
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    memcpy(bits, pbgra.data(), pbgra.size());
    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return bitmap;
}

}  // namespace imagecodec
