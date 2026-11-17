#include "tray_icon.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
using namespace std::literals;
constexpr int grid = 16;
constexpr int textHeight = 7;
constexpr int textTop = grid - textHeight;

struct NumberGlyph {
    int width;
    std::string_view pixels;
};

constexpr NumberGlyph digits[]{
    {4, " ## " "#  #" "#  #" "#  #" "#  #" "#  #" " ## "},
    {2, " #" "##" " #" " #" " #" " #" "##"},
    {4, " ## " "#  #" "   #" "  # " " #  " "#   " "####"},
    {4, "### " "   #" "   #" " ## " "   #" "   #" "### "},
    {4, "#  #" "#  #" "#  #" "####" "   #" "   #" "   #"},
    {4, "####" "#   " "#   " "### " "   #" "   #" "### "},
    {4, " ## " "#   " "#   " "### " "#  #" "#  #" " ## "},
    {4, "####" "   #" "  # " "  # " " #  " " #  " "#   "},
    {4, " ## " "#  #" "#  #" " ## " "#  #" "#  #" " ## "},
    {4, " ## " "#  #" "#  #" " ###" "   #" "   #" " ## "},
    {3, "## " "## " "  #" " # " "#  " " ##" " ##"},
};
static_assert(std::ranges::all_of(digits, [](const NumberGlyph& glyph) {
    return glyph.pixels.size() == static_cast<std::size_t>(glyph.width * textHeight);
}));

const NumberGlyph& number_glyph(char ch) {
    return digits[ch == '%' ? 10 : ch - '0'];
}

using ChargeLabel = std::array<std::array<bool, grid>, textHeight>;

ChargeLabel charge_label(unsigned percent) {
    const auto text = std::to_string(percent) + '%';
    int width = static_cast<int>(text.size()) - 1;
    for (char ch : text) width += number_glyph(ch).width;
    ChargeLabel mask{};
    int left = (grid - width) / 2;
    for (char ch : text) {
        const auto& glyph = number_glyph(ch);
        for (int y = 0; y < textHeight; ++y) {
            for (int x = 0; x < glyph.width; ++x)
                mask[y][left + x] = glyph.pixels[y * glyph.width + x] == '#';
        }
        left += glyph.width + 1;
    }
    return mask;
}

// The first 13 rows contain the glyph. The last rows are reserved for the charge bar.
constexpr auto mouse =
    "                "
    "      ####      "
    "     #  # #     "
    "    #   #  #    "
    "    #   #  #    "
    "    ########    "
    "    #      #    "
    "    #      #    "
    "    #      #    "
    "    #      #    "
    "     #    #     "
    "      ####      "
    "                "sv;

constexpr auto audio =
    "                "
    "      ####      "
    "    ##    ##    "
    "   #        #   "
    "  #          #  "
    "  #          #  "
    "  ##        ##  "
    "  ###      ###  "
    "  ###      ###  "
    "  ###      ###  "
    "   ##      ##   "
    "                "
    "                "sv;

constexpr auto keyboard =
    "                "
    "                "
    "                "
    "  ############  "
    " #            # "
    " # # # # # ## # "
    " #            # "
    " # # # # # ## # "
    " #            # "
    " # # ###### # # "
    "  ############  "
    "                "
    "                "sv;

constexpr auto controller =
    "                "
    "                "
    "                "
    "    ########    "
    "   #        #   "
    "  #  #    #  #  "
    "  # ###    # #  "
    " #   #  # #   # "
    " #            # "
    " #   ######   # "
    "  ###      ###  "
    "                "
    "                "sv;

constexpr auto battery =
    "                "
    "                "
    "                "
    "  ###########   "
    " #           #  "
    " #           ## "
    " #           ## "
    " #           ## "
    " #           #  "
    "  ###########   "
    "                "
    "                "
    "                "sv;

static_assert(mouse.size() == 13 * grid && audio.size() == mouse.size() &&
              keyboard.size() == mouse.size() && controller.size() == mouse.size() && battery.size() == mouse.size());

std::string_view glyph(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::mouse: return mouse;
    case DeviceKind::audio: return audio;
    case DeviceKind::keyboard: return keyboard;
    case DeviceKind::controller: return controller;
    case DeviceKind::other: return battery;
    }
    return battery;
}
} // namespace

HICON create_tray_icon(DeviceKind kind, std::optional<unsigned> percent, bool showPercent) {
    const int side = std::clamp(GetSystemMetrics(SM_CXSMICON), grid, 128);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = side;
    info.bmiHeader.biHeight = -side;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    const std::vector<unsigned char> maskBits(((side + 15) / 16) * 2 * side, 0);
    const HBITMAP mask = CreateBitmap(side, side, 1, 1, maskBits.data());
    void* pixels = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HICON icon = nullptr;
    if (mask && color && pixels) {
        const auto shape = glyph(kind);
        const auto filled = percent ? std::min(*percent, 100u) * 12u : 0;
        const bool drawPercent = showPercent && percent.has_value();
        const auto label = drawPercent ? charge_label(std::min(*percent, 100u)) : ChargeLabel{};
        auto* pixel = static_cast<std::uint32_t*>(pixels);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const int gx = x * grid / side;
                const int gy = y * grid / side;
                std::uint32_t shade = 0;
                if (drawPercent) {
                    if ((gy < textTop - 1 && shape[(1 + (gy * 10 + 3) / 7) * grid + gx] == '#') ||
                        (gy >= textTop && label[gy - textTop][gx])) shade = 0xffffffff;
                } else if (gy < 13 && shape[gy * grid + gx] == '#') {
                    shade = 0xffffffff;
                } else if (percent && gy == 14 && gx >= 2 && gx < 14) {
                    const auto barIndex = static_cast<unsigned>(gx - 2);
                    shade = (filled + 50 >= (barIndex + 1) * 100) ? 0xffffffff : 0x60606060;
                }
                *pixel++ = shade;
            }
        }
        ICONINFO iconInfo{TRUE, 0, 0, mask, color};
        icon = CreateIconIndirect(&iconInfo);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}
