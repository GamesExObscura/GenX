// What pixel smoothing actually costs, in milliseconds.
//
// Reported as slow on Sonic 3 and fine on Game Gear, which is the shape of a
// cost that scales with the WINDOW rather than with the console: a Mega Drive
// frame is 320x224 against the Game Gear's 160x144, but the destination is the
// same screen either way, so that cannot be the whole story. Guessing at it
// once already cost nine tenths of the frame rate, so this measures the exact
// path the runtime takes - GDI+ bilinear into a cached memory surface, then a
// blit - at the sizes that matter, with no emulator running underneath.
//
// A 60Hz frame is 16.67ms of which the Mega Drive core needs most; anything
// here above about 5ms is why the picture stutters.

#include <windows.h>
#include <d2d1.h>
#include <gdiplus.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    void* pixels = nullptr;

    bool create(HDC reference, int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(reference);
        if (dc == nullptr) {
            return false;
        }
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (bitmap == nullptr) {
            return false;
        }
        SelectObject(dc, bitmap);
        return true;
    }

    void destroy() {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (dc != nullptr) {
            DeleteDC(dc);
        }
    }
};

// One measurement: scale `src_w` x `src_h` up to `dst_w` x `dst_h` the way the
// runtime does it, `runs` times, and report the average.
double time_smooth(HDC screen, int src_w, int src_h, int dst_w, int dst_h, int runs,
                   bool with_attributes) {
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(src_w) * src_h);
    // Not a flat fill: a constant image would let a scaler skip work, and the
    // answer would be a number nothing in the real path can reach.
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            frame[static_cast<std::size_t>(y) * src_w + x] =
                static_cast<std::uint32_t>((x * 7 + y * 13) | ((x ^ y) << 8) | (y << 16));
        }
    }

    Surface target;
    if (!target.create(screen, dst_w, dst_h)) {
        return -1.0;
    }

    Gdiplus::Bitmap source(src_w, src_h, src_w * static_cast<INT>(sizeof(std::uint32_t)),
                           PixelFormat32bppRGB, reinterpret_cast<BYTE*>(frame.data()));
    if (source.GetLastStatus() != Gdiplus::Ok) {
        target.destroy();
        return -1.0;
    }

    const auto started = std::chrono::steady_clock::now();
    for (int run = 0; run < runs; ++run) {
        Gdiplus::Graphics graphics(target.dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const Gdiplus::Rect rect(0, 0, dst_w, dst_h);
        if (with_attributes) {
            // The clamp the runtime asks for, to stop the outermost pixels
            // blending against whatever lies past the edge of the frame.
            Gdiplus::ImageAttributes attributes;
            attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
            graphics.DrawImage(&source, rect, 0, 0, src_w, src_h, Gdiplus::UnitPixel, &attributes);
        } else {
            graphics.DrawImage(&source, rect, 0, 0, src_w, src_h, Gdiplus::UnitPixel);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    target.destroy();
    return std::chrono::duration<double, std::milli>(elapsed).count() / runs;
}

// The alternative worth costing: bilinear only as far as a bounded
// intermediate, then let GDI do the rest. The expensive part scales with the
// DESTINATION, so capping the destination caps the cost.
double time_two_stage(HDC screen, int src_w, int src_h, int mid_w, int mid_h, int dst_w, int dst_h,
                      int runs) {
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(src_w) * src_h);
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            frame[static_cast<std::size_t>(y) * src_w + x] =
                static_cast<std::uint32_t>((x * 7 + y * 13) | ((x ^ y) << 8) | (y << 16));
        }
    }

    Surface middle;
    Surface target;
    if (!middle.create(screen, mid_w, mid_h) || !target.create(screen, dst_w, dst_h)) {
        middle.destroy();
        target.destroy();
        return -1.0;
    }

    Gdiplus::Bitmap source(src_w, src_h, src_w * static_cast<INT>(sizeof(std::uint32_t)),
                           PixelFormat32bppRGB, reinterpret_cast<BYTE*>(frame.data()));
    if (source.GetLastStatus() != Gdiplus::Ok) {
        middle.destroy();
        target.destroy();
        return -1.0;
    }

    const auto started = std::chrono::steady_clock::now();
    for (int run = 0; run < runs; ++run) {
        Gdiplus::Graphics graphics(middle.dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes attributes;
        attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        const Gdiplus::Rect rect(0, 0, mid_w, mid_h);
        graphics.DrawImage(&source, rect, 0, 0, src_w, src_h, Gdiplus::UnitPixel, &attributes);
        SetStretchBltMode(target.dc, HALFTONE);
        StretchBlt(target.dc, 0, 0, dst_w, dst_h, middle.dc, 0, 0, mid_w, mid_h, SRCCOPY);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    middle.destroy();
    target.destroy();
    return std::chrono::duration<double, std::milli>(elapsed).count() / runs;
}

// The same scale on the graphics card, through Direct2D. Needs a real window,
// because an HWND render target is the thing that actually reaches the GPU - a
// DC or WIC target would quietly measure software again and report a number
// that means nothing.
double time_direct2d(int src_w, int src_h, int dst_w, int dst_h, int runs) {
    static const wchar_t* kClass = L"GenXSmoothBench";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = kClass;
        RegisterClassExW(&cls);
        registered = true;
    }
    // Positioned off the visible desktop rather than hidden: a hidden window
    // can be given a render target that never composites, which would measure
    // nothing.
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP, -32000, -32000, dst_w,
                                  dst_h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
        return -1.0;
    }
    ShowWindow(window, SW_SHOWNA);

    ID2D1Factory* factory = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory)) ||
        factory == nullptr) {
        DestroyWindow(window);
        return -1.0;
    }
    ID2D1HwndRenderTarget* target = nullptr;
    const auto properties = D2D1::RenderTargetProperties();
    // IMMEDIATELY, or EndDraw waits for the vertical blank and every size
    // measures the same ~31ms - the display's pace, not the scaler's. The
    // runtime wants the wait; a benchmark trying to find the cost does not.
    const auto hwnd_properties = D2D1::HwndRenderTargetProperties(
        window, D2D1::SizeU(dst_w, dst_h), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(factory->CreateHwndRenderTarget(properties, hwnd_properties, &target)) ||
        target == nullptr) {
        factory->Release();
        DestroyWindow(window);
        return -1.0;
    }

    std::vector<std::uint32_t> frame(static_cast<std::size_t>(src_w) * src_h);
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            frame[static_cast<std::size_t>(y) * src_w + x] =
                static_cast<std::uint32_t>((x * 7 + y * 13) | ((x ^ y) << 8) | (y << 16)) |
                0xFF000000u;
        }
    }

    ID2D1Bitmap* bitmap = nullptr;
    const auto format =
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
    if (FAILED(target->CreateBitmap(D2D1::SizeU(src_w, src_h),
                                    D2D1::BitmapProperties(format), &bitmap)) ||
        bitmap == nullptr) {
        target->Release();
        factory->Release();
        DestroyWindow(window);
        return -1.0;
    }

    const auto started = std::chrono::steady_clock::now();
    for (int run = 0; run < runs; ++run) {
        // The upload counts: a real frame changes every time, so measuring
        // only the draw would flatter it.
        bitmap->CopyFromMemory(nullptr, frame.data(),
                               static_cast<UINT32>(src_w * sizeof(std::uint32_t)));
        target->BeginDraw();
        target->DrawBitmap(bitmap,
                           D2D1::RectF(0.0f, 0.0f, static_cast<float>(dst_w),
                                       static_cast<float>(dst_h)),
                           1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target->EndDraw();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    bitmap->Release();
    target->Release();
    factory->Release();
    DestroyWindow(window);
    return std::chrono::duration<double, std::milli>(elapsed).count() / runs;
}

}  // namespace

int main() {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) {
        std::printf("GDI+ would not start\n");
        return 1;
    }
    HDC screen = GetDC(nullptr);

    struct Case {
        const char* label;
        int src_w;
        int src_h;
    };
    const Case cases[] = {
        {"Game Gear  160x144", 160, 144},
        {"Mega Drive 320x224", 320, 224},
        {"Mega Drive 320x240", 320, 240},
    };
    struct Target {
        const char* label;
        int w;
        int h;
    };
    const Target targets[] = {
        {"720p   1280x720 ", 1280, 720},
        {"1080p  1920x1080", 1920, 1080},
        {"1440p  2560x1440", 2560, 1440},
    };

    std::printf("One frame of pixel smoothing, milliseconds. 60Hz budget is 16.67ms\n");
    std::printf("and the Mega Drive core needs most of that.\n\n");
    std::printf("%-20s %-18s %10s %10s %10s\n", "console frame", "window", "GDI+", "capped 3x",
                "Direct2D");
    for (const auto& source : cases) {
        for (const auto& target : targets) {
            const double clamped = time_smooth(screen, source.src_w, source.src_h, target.w,
                                               target.h, 30, true);
            const double capped = time_two_stage(screen, source.src_w, source.src_h,
                                                 source.src_w * 3, source.src_h * 3, target.w,
                                                 target.h, 30);
            const double gpu = time_direct2d(source.src_w, source.src_h, target.w, target.h, 60);
            std::printf("%-20s %-18s %9.2f %10.2f %10.2f\n", source.label, target.label, clamped,
                        capped, gpu);
        }
    }

    ReleaseDC(nullptr, screen);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
