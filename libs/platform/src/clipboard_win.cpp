// The Windows half of the session clipboard: the user32
// Open/Close pair around CF_UNICODETEXT (text),
// RegisterClipboardFormatW (typed payloads - the type IS the OS
// format name) and CF_DIB (images). The clipboard is busy-shared
// with every other window: a refused open answers io_failed and
// retry is the caller's policy - never a spin here. Reads answer
// nullopt when the format is absent.

#include <cstring>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ws/platform/clipboard.hpp>
#include <ws/platform/errors.hpp>

#include "win_utf8.hpp"

namespace ws::platform {
namespace {

// OpenClipboard on construction, Close on every exit path. A
// clipboard held by another window answers ok() == false.
class ClipboardSession {
  public:
    ClipboardSession() : ok_{OpenClipboard(nullptr) != FALSE} {}
    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;
    ~ClipboardSession() {
        if (ok_) {
            CloseClipboard();
        }
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

  private:
    const bool ok_;
};

// A global block filled from the bytes, for SetClipboardData. The
// clipboard owns it after a successful set - callers free only
// what never left.
[[nodiscard]] HGLOBAL make_global(std::string_view bytes) {
    const HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (block == nullptr) {
        return nullptr;
    }
    void* dst = GlobalLock(block);
    if (dst == nullptr) {
        GlobalFree(block);
        return nullptr;
    }
    if (!bytes.empty()) {
        std::memcpy(dst, bytes.data(), bytes.size());
    }
    GlobalUnlock(block);
    return block;
}

// The typed-payload format id: the type name doubles as the OS
// format name, so two ws apps sharing a type interoperate.
[[nodiscard]] ws::core::Result<UINT> payload_format(
    std::string_view type) {
    if (type.empty()) {
        return fail(PlatformError::invalid_argument,
                    "clipboard: the payload type is empty");
    }
    const UINT format = RegisterClipboardFormatW(from_utf8(type).c_str());
    if (format == 0) {
        return fail(PlatformError::io_failed,
                    "clipboard: the type name did not register");
    }
    return format;
}

// GlobalLock held for the read, unlocked on every exit path. The
// block stays the clipboard's - only the lock is ours.
class LockedGlobal {
  public:
    explicit LockedGlobal(HGLOBAL block)
        : block_{block}, ptr_{GlobalLock(block)} {}
    LockedGlobal(const LockedGlobal&) = delete;
    LockedGlobal& operator=(const LockedGlobal&) = delete;
    ~LockedGlobal() {
        if (ptr_ != nullptr) {
            GlobalUnlock(block_);
        }
    }
    [[nodiscard]] const void* get() const noexcept { return ptr_; }
    [[nodiscard]] bool ok() const noexcept { return ptr_ != nullptr; }

  private:
    HGLOBAL block_ = nullptr;
    void* ptr_ = nullptr;
};

}  // namespace

ws::core::Status set_clipboard_text(std::string_view text) {
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (EmptyClipboard() == FALSE) {
        return fail(PlatformError::io_failed,
                    "clipboard: clearing the old content failed");
    }
    // CF_UNICODETEXT carries the terminator: one wide char past
    // the text.
    const std::wstring wide = from_utf8(text);
    const char* data = reinterpret_cast<const char*>(wide.c_str());
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    const HGLOBAL block = make_global(std::string_view{data, bytes});
    if (block == nullptr ||
        SetClipboardData(CF_UNICODETEXT, block) == nullptr) {
        if (block != nullptr) {
            GlobalFree(block);
        }
        return fail(PlatformError::io_failed,
                    "clipboard: placing the text failed");
    }
    return {};
}

ws::core::Result<std::optional<std::string>> clipboard_text() {
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) {
        return std::optional<std::string>{};  // no text: a normal answer
    }
    const HGLOBAL block = GetClipboardData(CF_UNICODETEXT);
    if (block == nullptr) {
        return fail(PlatformError::io_failed,
                    "clipboard: fetching the text failed");
    }
    const LockedGlobal locked{block};
    if (!locked.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: locking the text failed");
    }
    return to_utf8(static_cast<const wchar_t*>(locked.get()));
}

ws::core::Status set_clipboard_payload(std::string_view type,
                                       std::string_view bytes) {
    WS_TRY_ASSIGN(const UINT format, payload_format(type));
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (EmptyClipboard() == FALSE) {
        return fail(PlatformError::io_failed,
                    "clipboard: clearing the old content failed");
    }
    const HGLOBAL block = make_global(bytes);
    if (block == nullptr || SetClipboardData(format, block) == nullptr) {
        if (block != nullptr) {
            GlobalFree(block);
        }
        return fail(PlatformError::io_failed,
                    "clipboard: placing the payload failed");
    }
    return {};
}

ws::core::Result<std::optional<std::string>> clipboard_payload(
    std::string_view type) {
    WS_TRY_ASSIGN(const UINT format, payload_format(type));
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (IsClipboardFormatAvailable(format) == FALSE) {
        return std::optional<std::string>{};
    }
    const HGLOBAL block = GetClipboardData(format);
    if (block == nullptr) {
        return fail(PlatformError::io_failed,
                    "clipboard: fetching the payload failed");
    }
    const LockedGlobal locked{block};
    if (!locked.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: locking the payload failed");
    }
    // GlobalSize is the byte count the setter placed - the
    // payload channel carries its own length, no terminator.
    const std::string out{static_cast<const char*>(locked.get()),
                          GlobalSize(block)};
    return out;
}

ws::core::Status set_clipboard_image(const ClipboardImage& image) {
    if (image.width <= 0 || image.height <= 0) {
        return fail(PlatformError::invalid_argument,
                    "clipboard: the image is empty");
    }
    const size_t pixels = static_cast<size_t>(image.width) *
                              static_cast<size_t>(image.height) * 4;
    if (image.bytes.size() != pixels) {
        return fail(PlatformError::invalid_argument,
                    "clipboard: the byte count does not match the size");
    }
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (EmptyClipboard() == FALSE) {
        return fail(PlatformError::io_failed,
                    "clipboard: clearing the old content failed");
    }
    // CF_DIB: a BITMAPINFOHEADER then top-down 32-bit BGRA rows.
    // A negative biHeight means top-down (no row flip), and
    // 32-bit rows are already four-aligned - no padding pass.
    std::string dib(sizeof(BITMAPINFOHEADER) + pixels, '\0');
    auto* const header = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = image.width;
    header->biHeight = -image.height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixels);
    auto* const out = reinterpret_cast<std::uint8_t*>(
        dib.data() + sizeof(BITMAPINFOHEADER));
    for (size_t i = 0; i < pixels; i += 4) {
        out[i + 0] = static_cast<std::uint8_t>(image.bytes[i + 2]);
        out[i + 1] = static_cast<std::uint8_t>(image.bytes[i + 1]);
        out[i + 2] = static_cast<std::uint8_t>(image.bytes[i + 0]);
        out[i + 3] = static_cast<std::uint8_t>(image.bytes[i + 3]);
    }
    const HGLOBAL block = make_global(dib);
    if (block == nullptr || SetClipboardData(CF_DIB, block) == nullptr) {
        if (block != nullptr) {
            GlobalFree(block);
        }
        return fail(PlatformError::io_failed,
                    "clipboard: placing the image failed");
    }
    return {};
}

ws::core::Result<std::optional<ClipboardImage>> clipboard_image() {
    const ClipboardSession clipboard;
    if (!clipboard.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: held by another window");
    }
    if (IsClipboardFormatAvailable(CF_DIB) == FALSE) {
        return std::optional<ClipboardImage>{};
    }
    const HGLOBAL block = GetClipboardData(CF_DIB);
    if (block == nullptr) {
        return fail(PlatformError::io_failed,
                    "clipboard: fetching the image failed");
    }
    const LockedGlobal locked{block};
    if (!locked.ok()) {
        return fail(PlatformError::io_failed,
                    "clipboard: locking the image failed");
    }
    const size_t stored = GlobalSize(block);
    if (stored < sizeof(BITMAPINFOHEADER)) {
        return fail(PlatformError::io_failed,
                    "clipboard: the DIB is truncated");
    }
    const auto& header =
        *static_cast<const BITMAPINFOHEADER*>(locked.get());
    if (header.biBitCount != 32 || header.biCompression != BI_RGB) {
        return fail(PlatformError::io_failed,
                    "clipboard: the DIB is not a plain 32-bit one");
    }
    const bool top_down = header.biHeight < 0;
    ClipboardImage image;
    image.height = top_down ? -header.biHeight : header.biHeight;
    image.width = header.biWidth;
    if (image.width <= 0 || image.height <= 0) {
        return fail(PlatformError::io_failed,
                    "clipboard: the DIB is empty");
    }
    const size_t pixels = static_cast<size_t>(image.width) *
                              static_cast<size_t>(image.height) * 4;
    if (stored < sizeof(BITMAPINFOHEADER) + pixels) {
        return fail(PlatformError::io_failed,
                    "clipboard: the DIB pixel data is truncated");
    }
    // The BGRA rows land as RGBA; a bottom-up DIB (the older
    // direction) stores the last row first, so it is read back
    // into top-down order here.
    const auto* const in =
        static_cast<const std::uint8_t*>(locked.get()) +
        sizeof(BITMAPINFOHEADER);
    const std::int64_t stride = static_cast<std::int64_t>(image.width) * 4;
    image.bytes.resize(pixels);
    auto* const out = reinterpret_cast<std::uint8_t*>(image.bytes.data());
    for (std::int32_t row = 0; row < image.height; ++row) {
        const std::int32_t source =
            top_down ? row : image.height - 1 - row;
        const auto* const src =
            in + static_cast<std::int64_t>(source) * stride;
        auto* const dst = out + static_cast<std::int64_t>(row) * stride;
        for (std::int64_t x = 0; x < stride; x += 4) {
            dst[x + 0] = src[x + 2];
            dst[x + 1] = src[x + 1];
            dst[x + 2] = src[x + 0];
            dst[x + 3] = src[x + 3];
        }
    }
    return image;
}

}  // namespace ws::platform
