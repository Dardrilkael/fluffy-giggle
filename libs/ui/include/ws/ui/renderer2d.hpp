#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>  // ws::core::Status (font_add_face) - a
                               // workspace header, not a third-party one

namespace ws::ui::r2d {

// 2D renderer: batched quads, lines and text, textured or flat. Apps record draws
// from on_update() only, exactly like the widgets.hpp calls; the shell owns
// every GPU detail (pipeline, buffers, flush point - 2D renders under the
// widget layer, command-queue overlays render on top). Draws are recorded
// CPU-side during on_update() and never outlive their frame: a frame that
// can't render drops them, it never defers them (same contract as the
// render command queue). Every call is drop-safe outside a running app -
// logged once, then ignored.

// --- camera ---------------------------------------------------------------------
// World-space camera. x/y = the world point shown at the window's CENTER;
// rotation = radians, positive rolls the view clockwise on screen; zoom =
// magnification (2 shows half the world span; 1 makes world units equal
// framebuffer pixels). World +y renders downward, matching screen coords.
struct Camera2D {
    float x = 0.0f;
    float y = 0.0f;
    float rotation = 0.0f;
    float zoom = 1.0f;
};

// 3D perspective camera for the same y-down world the 2D camera draws in:
// world -y renders UP on screen, exactly like every 2D draw. eye and target
// are world points; up is the direction that must appear up on screen (the
// default {0,-1,0} keeps the y-down convention); fov_y is the vertical field
// of view in radians; near_z and far_z clip along the view axis. Plain
// floats, like Camera2D - and usable out of the box the same way: the
// default looks down +z at the origin from 400 units back.
struct Camera3D {
    float eye[3] = {0.0f, 0.0f, -400.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, -1.0f, 0.0f};
    float fov_y = 0.7853982f;  // 45 degrees
    float near_z = 0.1f;
    float far_z = 1000.0f;
};

// --- textures ---------------------------------------------------------------------
// Opaque handle. 0 = the built-in 1x1 white texture (also the fallback for
// failed loads) - it is never destroyed. Every other handle is
// generation-counted: destroy_texture() kills it the moment it returns
// (further draws render white, warned once per frame) and reclaims the GPU
// memory once the frames in flight have drained; the freed slot is what a
// later load reuses, and a stale handle kept past that fails the
// generation check and renders white. The 12-bit generation wraps after
// 4096 reuses of one slot - a handle older than that can false-positive
// as fresh (documented limit).
struct Texture {
    std::uint32_t id = 0;
};

// An atlas sprite: one sub-rect of one packed atlas page (load_atlas).
// page is the page texture's id, so every sprite of a page shares one
// batch material no matter how interleaved the draw calls are; index
// selects the sub-rect. {} is the white sentinel failed entries degrade
// to - it draws plain white, like Texture{}.
struct Sprite {
    std::uint32_t page = 0;
    std::uint32_t index = 0;
};

// Packs channels as 0xRRGGBBAA. NOT ImGui's IM_COL32 order - don't paste
// ImGui color literals into r2d calls.
inline constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g,
                                    std::uint8_t b, std::uint8_t a = 255) {
    return (std::uint32_t(r) << 24) | (std::uint32_t(g) << 16) |
           (std::uint32_t(b) << 8) | std::uint32_t(a);
}

// Framebuffer size in pixels - the space the 2D world is defined in.
// WindowResizeEvent carries *screen* coordinates, which differ under DPI
// scaling; this reports what the frame actually renders at. {0, 0} when no
// app is running.
struct FrameSize {
    float width = 0.0f;
    float height = 0.0f;
};
FrameSize frame_size();

// --- frame recording ----------------------------------------------------------------
// Screen mode: world (0,0) = window top-left, 1 world unit = 1 framebuffer
// pixel. Every begin() opens a new camera block, so calling them repeatedly
// per frame layers blocks back to front.
//
// At the frame's end the shell sorts the recorded draws - explicit layer
// first, then camera block - and only then batches them. The sort is
// stable, and neither texture nor clip is a key: draws that share layer
// and camera block replay in their exact submission order - real
// painter's order - whatever textures they use (one chunk holds many
// textures alongside, so mixed textures neither reorder nor split the
// chunk). A clip change splits a fresh chunk in record order
// (one chunk = one scissor), so clipped regions keep painter order too:
// two overlapping panels render in the order the app drew them, never
// reordered by their clip rects.
// set_layer() stays the explicit z escape hatch when overlap order must
// disagree with submission order.
void begin();
void begin(const Camera2D& camera);

// Opens a 3D perspective camera block (the struct above). Inside it the 3D
// overloads below - the draw_quad/draw_sprite/draw_line forms that take a
// z - record; 2D draws are legal too, but land on the world's z=0 plane
// and render through this camera (surprising; prefer separate blocks). A
// 3D overload called inside a 2D block warns once per frame and records
// nothing. near_z/far_z clip along the view axis, the projection's own
// clipping; push_clip stays framebuffer-space and applies unchanged. 3D
// blocks sort by submission and layer exactly like 2D blocks - one block
// is one kind, so blocks never interleave mid-batch.
void begin(const Camera3D& camera);

// Ends the current camera block. Optional - an unclosed block flushes
// as-is - but draws between end() and the next begin() are dropped.
void end();

// Sorts all subsequent draws into `layer` (default 0). Higher layers draw
// later - on top - regardless of submission order or camera block.
// Frame-scoped like the clip stack: frame boundaries reset it to 0, so a
// scope that wants a layer re-establishes it each frame (the gui's
// begin_layer/end_layer pairs already do). The gui's layer strata
// (ws/ui/gui: Layer, 10..40) are values in this same space, driven by
// GuiContext::begin_layer - one numbering, both sides.
void set_layer(std::uint32_t layer);

// Pushes a framebuffer-space clip rect (top-left origin, pixels): draws
// recorded until the matching pop_clip() render only inside the rect
// intersected with the clips already active - nesting always shrinks.
// Clips are screen-space even inside camera blocks (the GPU scissors
// framebuffer pixels), and they batch: draws under one clip group
// together and apart from other clips, each group flushing under its own
// scissor. The stack empties at each frame's start, so a clip cannot
// leak across frames; pushes past the depth cap keep pop_clip() balanced
// (deeper clips stop shrinking, warned once). pop_clip() with nothing
// pushed is a no-op.
void push_clip(float x, float y, float width, float height);
void pop_clip();

// The overlay spelling of push_clip: pushes `rect` as the new innermost
// clip WITHOUT intersecting the clips already active. A panel nesting
// inside another panel shrinks (push_clip always intersects) - but an
// overlay that floats OVER a panel must draw past its host's clip, or a
// dropdown near the panel's edge would shear off at it. The escape is
// exactly that: from here to the matching pop_clip() the active clip is
// this rect alone, and further nesting from inside intersects with it
// as usual. The gui's popup/tooltip/modal scopes are the intended
// callers (and the gui's hit-testing mirror, push_hit_clip_escape, is
// spelled to match). Same normalization, depth cap and no-op pop
// contract as push_clip.
void push_clip_escape(float x, float y, float width, float height);

// Top-left anchored quad; rotation pivots the center (positive = clockwise,
// radians). Flat colors pass texture = {}.
void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture = {}, float rotation = 0.0f);

// 3D form (inside a begin(Camera3D) block only): a camera-facing billboard,
// CENTER-anchored at (x, y, z) - w/h are world units spread along the
// camera's screen axes, so the quad always faces the view and shrinks with
// distance. No rotation in v1. In a 2D block: warns once, records nothing.
void draw_quad(float x, float y, float z, float w, float h,
               std::uint32_t color, Texture texture = {});

// Draws an atlas sprite: same quad contract as draw_quad, but the texture
// is the sprite's page and the UVs are its sub-rect - a page's sprites all
// batch into one segment. Rotation pivots the center. Unknown or sentinel
// sprites draw white.
void draw_sprite(float x, float y, float w, float h, std::uint32_t color,
                 Sprite sprite, float rotation = 0.0f);

// 3D form: the same billboard contract as the 3D draw_quad - camera-facing,
// center-anchored, w/h in world units - textured with the sprite's page
// sub-rect. Unknown or sentinel sprites draw white, the 2D contract.
void draw_sprite(float x, float y, float z, float w, float h,
                 std::uint32_t color, Sprite sprite);

// Quad of `thickness` pixels spanning the two endpoints (axis-crossed).
// Degenerate input (zero length, non-positive thickness) is dropped
// silently.
void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color);

// 3D form (inside a begin(Camera3D) block only): a ribbon of `thickness`
// WORLD units (not pixels) spanning the two world endpoints, expanded
// perpendicular to the segment and the view - constant world width along
// its length. Degenerate input (zero length, non-positive thickness) is
// dropped silently, the 2D contract.
void draw_line(float x0, float y0, float z0, float x1, float y1, float z1,
               float thickness, std::uint32_t color);

// Free-form vertex-colored quad - the canvas layer's tessellation sink
// (ws::ui::canvas draws everything through it). Four corners in the
// current camera's space, 2D blocks only (inside a begin(Camera3D) block
// it warns once per frame and records nothing, the reverse of the 3D
// overloads' rule); a PER-CORNER rgba of rasterize-ready floats 0..1 -
// not packed, not clamped here (record_vcol clamps before packing); and
// caller-managed UVs with an optional texture ({} = flat color). Winding
// is the caller's to keep consistent: both index triangles (0,1,2 /
// 2,3,0) carry the corner order as named, and a TRIANGLE is spelled as a
// quad whose fourth corner duplicates the third (degenerate, zero-area -
// the fixed index pattern needs no triangle path). Same layer/clip/
// camera-block contract as every other draw; degenerate input (coincident
// corners making zero area) is not dropped - a caller tessellating a
// fan's last wedge relies on it landing where it lands.
void draw_vertex_quad(const float (&pos)[4][2], const float (&uv)[4][2],
                      const float (&rgba)[4][4], Texture texture = {});

// Flat-color rounded rectangle: ONE quad shaded by an analytic rounded-box
// SDF in the fragment shader (the radius rides the batch's per-vertex
// channel - no mask texture, so the radius is exact and antialiased at any
// camera zoom or rotation). The radius clamps to half the shorter side
// (past it the corners would overlap); a sub-pixel radius renders as the
// plain square quad. Rotation pivots the rect's center like draw_quad's.
void draw_rounded_quad(float x, float y, float w, float h, float radius,
                       std::uint32_t color, float rotation = 0.0f);

// Loads an image file (PNG/JPG/..., whatever stb_image reads) and returns
// its handle. Valid from on_setup() onward. On failure logs and returns the
// white texture. Loading is synchronous - fine at load granularity. One
// cap remains (M9's bindless tier): 1024 live textures per process - past
// it the load warns once and hands back white. Destroyed textures' slots
// return to a freelist later loads reuse (see destroy_texture).
Texture load_texture(const char* path);

// The in-memory spelling of load_texture: same decoder, same contract
// (white on failure, synchronous, valid from on_setup()). The buffer is
// read only during the call.
Texture load_texture_from_memory(const void* data, std::size_t size);

// Frees a texture. The handle is dead from this call on: further draws
// with it render white (warned once per frame), and the GPU memory is
// reclaimed at the flush kMaxFramesInFlight frames later - the frames
// still in flight when destroy_texture was called are the last that can
// see the old image. The freed slot is reused by later loads; a stale
// handle (destroyed already, or its slot since reused) is a warned no-op,
// and so is a second destroy of the same handle. id 0 (white) is
// refused. Atlas pages free the same way (Texture{sprite.page}); their
// sprites render white afterward.
void destroy_texture(Texture texture);

// Packs the given image files into atlas page textures (2048x2048, shelf
// packed tallest-first, duplicated-edge gutters) and returns one Sprite
// per input, in input order. All sprites on a page share that page's
// texture, so interleaved sprite draws stay in one batch segment; pages
// spill when full and batch apart, degrading gracefully. Missing/failed
// images become Sprite{} (white), like load_texture. Valid from on_setup()
// onward; loading is synchronous.
std::vector<Sprite> load_atlas(std::initializer_list<const char*> paths);

// The sprite's original pixel size - atlas packing never scales. {0, 0}
// for unknown or sentinel sprites.
FrameSize sprite_size(Sprite sprite);

// --- fonts + text ---------------------------------------------------------------------
// A baked font: one typeface rasterized once, at ONE pixel size - the v1
// rule is a Font per (face, size) pair, so a new size means a new bake,
// never scaling bitmaps (blurry) or re-rasterizing mid-frame (stutter).
// load_font() does the baking (pure CPU, stb_truetype - PRIVATE like
// every third-party) and gives the glyphs their OWN atlas pages: packing
// is one-shot, so a font never shares a page with load_atlas sprites,
// and two fonts are two page sets. A glyph is then an ordinary Sprite
// and draw_text() is a per-glyph draw_sprite loop - text carries the
// same layer, clip and camera-block contract as every other draw, and
// one page means one batch texture slot. v1 bakes ASCII 32..126 plus
// one synthesized fallback box: a codepoint outside the bake draws the
// box and advances it (a font built with fewer glyphs keeps the
// fallback zeroed - the old skip), and there is no kerning: a line
// measures as its plain advance sum.
//
// The growing font: a Font can STACK more typeface files under its
// primary bake (font_add_face) and bake their glyphs LAZILY -
// font_request_text marks a string's out-of-bake codepoints pending,
// font_bake_pending rasterizes a batch per frame into FRESH pages (pack
// is one-shot, so each batch is its own page set). The inline 95 stay
// the fast path; a codepoint past 126 resolves through the lazily-baked
// table before the fallback box, so draw_text, measure_text and every
// widget survive unchanged. One font stays ONE size - a runtime size
// change is a new font (its lazy glyphs re-request on first use). The
// stack and the table are the font's shared out-of-line half: a copy of
// a Font IS the same font - it draws what the original bakes, and a
// face added to either shows in both. Independent fonts come from
// load_font, never from a copy.
namespace font {
struct FontExtra;  // internal (src/text/font.h): the stack + the lazy table
}
class Font {
public:
    // One baked glyph. `sprite` is where its bitmap landed on the font's
    // pages; Sprite{} means nothing to draw (the space - it still
    // advances). The offsets place the bitmap relative to the pen and
    // the LINE TOP: (x + offset_x, y + offset_y) is the quad's top-left
    // when drawing at pen x on the line whose box starts at y.
    struct Glyph {
        Sprite sprite{};
        float advance = 0.0f;   // pen advance after this glyph, px
        float offset_x = 0.0f;  // bitmap left, relative to the pen
        float offset_y = 0.0f;  // bitmap top, relative to the line top
        float width = 0.0f;     // bitmap size, px (0 = nothing to draw)
        float height = 0.0f;
    };

    // Both constructors eagerly allocate the shared out-of-line half (the
    // copy contract above). Copies and moves stay implicit - a copy
    // shares, a move hands over, and neither ever deep-copies.
    Font();

    // Fill constructor (load_font's assembly step, and tests building
    // synthetic fonts): the glyphs for codepoints 32..126, in order,
    // then the fallback. Fewer than 96 entries leave the tail unbaked -
    // a missing fallback keeps the old skip; null bakes nothing.
    Font(float pixel_height, float line_height, const Glyph* glyphs,
         std::uint32_t count);

    // False when nothing was baked (a failed load, the headless
    // fallback): draw_text then draws nothing and measures 0.
    bool empty() const { return line_height_ <= 0.0f; }
    float pixel_height() const { return pixel_height_; }
    // The typeface's line advance at the baked size (ascent - descent +
    // line gap) - the height of the box draw_text's y names the top of.
    float line_height() const { return line_height_; }

    // The glyph for a codepoint: null below 32 (the controls), the
    // bake's own entry for 32..126, and past 126 the lazily-baked table
    // first (a baked entry wins, even headless-metric-only) then the
    // fallback box - where a blank glyph (advance 0, no sprite) is
    // returned as-is and draws and advances nothing, the same skip by
    // data (a zeroed fallback is that skip again).
    const Glyph* glyph(std::uint32_t codepoint) const;

    // The width draw_text advances for utf8_text: the glyphs' advance
    // sum - the fallback's included when the font carries one.
    // draw_text returns exactly this value.
    float measure_text(const char* utf8_text) const;

    // Internal (src/): the shared growing half behind glyph() and the
    // font_* free functions. The type is internal, so app code can
    // neither name it nor dereference this - it has no src/ include
    // path.
    font::FontExtra* extras_state() const noexcept { return extras_.get(); }

private:
    static constexpr std::uint32_t kFirstBaked = 32;
    static constexpr std::uint32_t kBakedCount = 95;  // ASCII 32..126
    // Slot kBakedCount, one past the bake: the fallback box every
    // codepoint outside 32..126 resolves to (the lazy table first).
    static constexpr std::uint32_t kFallbackGlyph = kBakedCount;
    Glyph glyphs_[kFallbackGlyph + 1]{};
    float pixel_height_ = 0.0f;
    float line_height_ = 0.0f;
    // Never null (every constructor allocates it): the face stack, the
    // lazily-baked table, the pending queue - see font.h's FontExtra.
    std::shared_ptr<font::FontExtra> extras_;
};

// Rasterizes the ttf/otf at `path` at one pixel height and returns the
// baked font. Valid from on_setup(), synchronous, like the texture
// loads. The path is the CALLER's to resolve - the library knows nothing
// about platforms or font directories; picking a face (the system one
// included) is app code, by design. On failure - missing file, not a
// font, no GPU context - logs once and returns an empty Font.
Font load_font(const char* path, float pixel_height);

// The face stack's failure codes (font_add_face): every failure is a
// ws::core::Error with category "ui" and one of these as its code.
// Append-only: never renumber, only add.
enum class FontError : std::int32_t {
    font_empty = 1,        // faces stack on a baked font - this one was not
    face_load_failed = 2,  // the file is missing, unreadable or not a font
};

// One-line description per code (the theme-json shape).
[[nodiscard]] constexpr std::string_view describe(FontError code) noexcept {
    switch (code) {
        case FontError::font_empty: return "the font was never baked";
        case FontError::face_load_failed:
            return "the face file could not be loaded";
    }
    return "unknown font error";
}

[[nodiscard]] inline ws::core::Error make_font_error(FontError code,
                                                     std::string message) {
    return ws::core::make_error(static_cast<std::int32_t>(code), "ui",
                                std::move(message));
}

// unexpected<Error> that converts into any Result<T> (the jobs shape):
//   return fail(FontError::font_empty, "font_add_face: no path");
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    FontError code, std::string message = {}) {
    if (message.empty()) {
        message = std::string{describe(code)};
    }
    return ws::core::unexpected<ws::core::Error>(
        make_font_error(code, std::move(message)));
}

// Stacks one more typeface file onto the font, as a fallback for
// codepoints past the inline bake. Resolution is per codepoint, inside
// the one font: lower `priority` value answers first, ties keep addition
// order; a face bakes at the font's own pixel height (fonts are one
// size), and its glyphs sit on the font's shared baseline. The same file
// may stack twice at different priorities. Fails - FontError - on a
// font that was never baked, and on a missing/invalid file; a failure
// leaves the stack untouched. Owner thread, like every Font call; the
// path is the caller's to resolve, exactly like load_font's.
ws::core::Status font_add_face(Font& font, const char* path, int priority);

// Marks every codepoint of the text that nobody can draw YET - past the
// inline bake, not in the lazy table, not already queued, and carried by
// some stacked face - as pending for font_bake_pending. Returns how many
// were newly marked; re-requesting the same text marks nothing more.
// A request made BEFORE a face was added found nobody carrying, so
// re-request after font_add_face - marking is cheap and idempotent, and
// the natural cadence is once per content change.
std::uint32_t font_request_text(Font& font, const char* utf8_text);

// Bakes up to batch_cap pending glyphs (request order) into fresh atlas
// pages - one upload per page - and returns how many landed. The rest
// stay pending for the next call: the amortized no-hitch contract, so
// the caller's cadence is once per frame with a cap that fits a frame.
// Headless (no live renderer) it still bakes the metrics - sprites stay
// Sprite{} and draw_text drops the quad but keeps the advance - so
// layout is testable before the first frame, and a bake never warns:
// filling the font IS the job, the page upload is the half that waits.
// A pending codepoint whose face stopped carrying it (or that baked to
// nothing at all) quietly leaves the queue without an entry - the
// fallback box keeps answering it. Owner thread, like every Font call.
std::uint32_t font_bake_pending(Font& font, std::uint32_t batch_cap = 64u);

// Draws one line of UTF-8 text, one sprite-quad per visible glyph.
// (x, y) is the LINE BOX's top-left - not the baseline: widgets center
// the box on their rows, and glyph offsets already reach down to the
// baseline from y. Returns the pen's total advance - exactly
// measure_text(utf8_text), by construction. The space and other blanks
// advance without drawing; codepoints past the bake draw the fallback
// box and advance it - until their lazy bake lands (font_bake_pending),
// after which they draw like any baked glyph - or, on a font whose
// fallback is zeroed, skip entirely; an empty font draws nothing and
// returns 0. On an
// axis-aligned, zoom-1 2D camera each glyph quad rounds to whole
// pixels - the pen keeps its exact fractional advance - while rotated
// or zoomed cameras keep fractional positions, so camera motion stays
// smooth. The bitmaps are white with coverage in alpha, so `color`
// tints the glyphs directly.
float draw_text(float x, float y, const char* utf8_text, const Font& font,
                std::uint32_t color = 0xFFFFFFFFu);

// --- diagnostics -----------------------------------------------------------------
// The last frame's accounting plus the registry state at call time.
// Zeros until the first frame ends (and every field reads zero with no
// app running); the registry/tier fields are empty by construction on a
// headless renderer.
struct Stats {
    std::uint32_t commands = 0;        // draws recorded into the last frame
    std::uint32_t quads = 0;           // quads the replay pushed
    std::uint32_t chunks = 0;          // batch chunks the replay built
    std::uint32_t vertices = 0;        // batch vertices (4 per quad)
    std::uint32_t dropped_quads = 0;   // recorded with no open segment
    std::uint32_t textures = 0;        // live: registry minus freed slots
    bool bindless = false;             // the device probe's texture tier
    bool update_after_bind = false;    // ...and its update-after-bind feature
};
Stats stats();

}  // namespace ws::ui::r2d
