// apps/voxel_game - test_voxel_frame_gpu.cpp: the app's OWN frame,
// rendered headless and READ BACK, pixel by pixel.
//
// The user's field report (round 2): "still not rendering correctly."
// The previous fix was verified by unit math, not by an actual frame;
// this harness closes that gap. It builds the real rig (headless Vulkan
// surface, offscreen render pass, readback - the test_ui_mesh_gpu
// template), generates the REAL world (the app's seed), rebuilds the
// REAL chunk meshes, spawns the REAL player, and renders the REAL frame
// path: sky quad -> voxel::begin_scene + voxel::draw_world -> (no HUD:
// text would only blur the pixels under study).
//
// Then it asks the pixels questions with numeric answers:
//   - is there terrain at all (non-sky coverage), or a blank sky?
//   - does the horizon sit where the world says it should?
//   - is the terrain lit, not black, not the sky color?
//   - when yaw turns east (+pi/2), does the view slide the way a
//     correct right turn slides it? Measured by cross-correlating the
//     two frames' terrain bands - THE question behind both field
//     reports, answered by displacement, not by eyeballs.
//
// WS_GPU_TEST_DUMP=<dir> dumps each frame as a .ppm for inspection.
// Opt-in like ui_mesh_gpu: WS_VOXEL_GPU_TESTS=ON; a missing Vulkan
// device SKIPS (message, not failure).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "renderer.hpp"

#include <array>
#include "world.hpp"

#include "render/renderer2d.h"

#include <ws/core/log.hpp>
#include <ws/gfx/gpu.hpp>
#include <ws/physics/3d/world.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using ws::ui::r2d::Camera3D;
using ws::ui::r2d::Renderer2D;
using ws::ui::r2d::TextureTier;
using namespace ws::ui::r3d;

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

// The app's own constants, spelled once here (main.cpp's kSeed, the
// center spawn cell) so the harness renders THE shipped scene.
constexpr std::uint32_t kSeed = 0xC0FFEEu;
constexpr int kSpawnX = voxel::kWorldSizeX / 2;
constexpr int kSpawnZ = voxel::kWorldSizeZ / 2;

// The sky the app paints: rgba(135, 196, 250).
constexpr int kSkyR = 135, kSkyG = 196, kSkyB = 250;

// Counts validation messages, the ui_mesh_gpu way (any real warning or
// error fails the case; the "validation requested but" note means the
// layer is absent, which gets a loud MESSAGE instead of silence).
class ValidationCounter final : public ws::core::LogSink {
public:
    void write(const ws::core::LogRecord& record) override {
        if (record.level < ws::core::LogLevel::warn) {
            return;
        }
        if (record.message.find("validation requested but") !=
            std::string_view::npos) {
            layer_missing = true;
        } else if (record.message.find("gfx: validation:") !=
                   std::string_view::npos) {
            ++count;
            if (first.empty()) {
                first = std::string(record.message);
            }
        }
    }
    std::atomic<int> count{0};
    std::atomic<bool> layer_missing{false};
    std::string first;
};

struct Rgba {
    int r = 0, g = 0, b = 0;
};

// The whole offscreen rig: device, render pass, color+depth targets,
// readback buffer, command buffer, a Renderer2D, plus the app's world
// and player. One rig renders many frames (the yaw turn) - meshes are
// created once at setup, the way on_setup() does it.
class Rig {
public:
    bool up = false;

    Rig() {
        counter_ = std::make_shared<ValidationCounter>();
        ws::core::add_sink(counter_);
        static const char* const kExts[] = {"VK_KHR_surface",
                                            "VK_EXT_headless_surface"};
        ws::gfx::SurfaceSource source;
        source.instance_extensions = kExts;
        source.instance_extension_count = 2;
        source.create_surface = [](void* instance, std::uint64_t* out, void*) {
            VkHeadlessSurfaceCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (vkCreateHeadlessSurfaceEXT(static_cast<VkInstance>(instance),
                                           &info, nullptr, &surface) != VK_SUCCESS) {
                return false;
            }
            *out = ws::gfx::handle_to_u64(surface);
            return true;
        };
        ws::gfx::DeviceOptions options;
        options.enable_validation = true;
        options.want_bindless = true;
        options.bindless_descriptor_limit = 1024;
        if (!gpu_.init(source, options) || !gpu_.attach_surface()) {
            return;
        }
        device_ = static_cast<VkDevice>(gpu_.device());
        queue_ = static_cast<VkQueue>(gpu_.queue());
        phys_ = static_cast<VkPhysicalDevice>(gpu_.physical_device());
        family_ = gpu_.queue_family();
        if (!build_targets()) {
            return;
        }
        TextureTier tier{};
        tier.bindless = gpu_.descriptor_indexing().bindless;
        tier.update_after_bind = gpu_.descriptor_indexing().update_after_bind;
        renderer_ = std::make_unique<Renderer2D>();
        if (!renderer_->init(device_, phys_, gpu_, queue_, family_, pass_,
                             VK_FORMAT_R8G8B8A8_UNORM, tier)) {
            return;
        }
        // The app's frame path speaks the FREE functions (r2d::begin,
        // r3d::create_mesh), and those route through the engine's
        // active-renderer pointer - App::run()'s own spell. The harness
        // has no App, so it sets the pointer itself (the same seam the
        // ui tests' stubs use).
        ws::ui::r2d::set_active(renderer_.get());
        if (!build_scene()) {
            return;
        }
        up = true;
    }

    ~Rig() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
        voxel_renderer_.destroy_all();  // GPU meshes die before Renderer2D
        world_.reset();
        ws::ui::r2d::set_active(nullptr);  // no dangling active pointer
        if (renderer_) {
            renderer_->shutdown();
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fence_, nullptr);
            vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyFramebuffer(device_, framebuffer_, nullptr);
            vkDestroyImageView(device_, color_view_, nullptr);
            vkDestroyImageView(device_, depth_view_, nullptr);
            gpu_.destroy_image(ws::gfx::handle_to_u64(color_), color_alloc_);
            gpu_.destroy_image(ws::gfx::handle_to_u64(depth_), depth_alloc_);
            gpu_.destroy_buffer(readback_buf_, readback_alloc_);
            vkDestroyRenderPass(device_, pass_, nullptr);
        }
        gpu_.shutdown();
        ws::core::remove_sink(counter_.get());
    }

    Renderer2D& r() { return *renderer_; }
    int validation_messages() const {
        if (counter_->layer_missing) {
            MESSAGE("Khronos validation layer NOT active - the zero below proves nothing");
        }
        return counter_->count;
    }
    voxel::World& world() { return *world_; }
    voxel::Player& player() { return player_; }

    // The app's own frame spell, verbatim from main.cpp's on_update()
    // (minus the HUD): sky, then begin_scene + draw_world.
    void render_frame(const char* dump_name) {
        Renderer2D& r = *renderer_;
        r.frame_begin(static_cast<float>(kW), static_cast<float>(kH));

        ws::ui::r2d::begin();  // the 2D epoch: the sky quad
        ws::ui::r2d::set_layer(0);
        ws::ui::r2d::draw_quad(0.0f, 0.0f, static_cast<float>(kW),
                               static_cast<float>(kH),
                               ws::ui::r2d::rgba(135, 196, 250));
        ws::ui::r2d::end();

        const auto cam = voxel::begin_scene(player_);
        voxel::draw_world(voxel_renderer_, tiles_, cam,
                          static_cast<float>(kW) / static_cast<float>(kH));
        ws::ui::r2d::end();

        r.frame_end();
        submit_and_read(dump_name);
    }

    Rgba px(std::uint32_t x, std::uint32_t y) const {
        const unsigned char* p = &pixels_[(y * kW + x) * 4];
        return {p[0], p[1], p[2]};
    }

    // True when the pixel is the sky color (small tolerance for the
    // rasterizer's rounding).
    bool is_sky(std::uint32_t x, std::uint32_t y) const {
        const Rgba p = px(x, y);
        return std::abs(p.r - kSkyR) <= 3 && std::abs(p.g - kSkyG) <= 3 &&
               std::abs(p.b - kSkyB) <= 3;
    }

    // A copy of the current frame's pixels (for cross-frame measures).
    std::vector<unsigned char> pixels_copy() const { return pixels_; }
    // Loads a saved frame back for px()/is_sky().
    void load_pixels(const std::vector<unsigned char>& p) { pixels_ = p; }

private:
    // The scene: world + player, the app's on_setup() spell (no audio,
    // no physics shells). The player's body lives in a throwaway
    // physics world - spawn() needs one to create the body, and the
    // tick below needs a steppable one; the harness never advances it,
    // so the pose is exactly the spawn pose.
    bool build_scene() {
        world_ = std::make_unique<voxel::World>();
        world_->generate(kSeed);
        ws::physics3d::World3D::Desc desc{};
        desc.gravity = {0.0f, -32.0f, 0.0f};
        desc.fixed_dt = 1.0f / 60.0f;
        desc.thread_count = 0;
        desc.max_bodies = 8;  // just the player, nothing else lives here
        auto phys = ws::physics3d::World3D::create(desc);
        if (!phys.has_value()) {
            return false;
        }
        const int surface = world_->surface_height(kSpawnX, kSpawnZ);
        const ws::math::Vec3 spawn{
            static_cast<float>(kSpawnX) + 0.5f,
            static_cast<float>(surface) + 1.0f + player_.tuning_half_height(),
            static_cast<float>(kSpawnZ) + 0.5f};
        if (!player_.spawn(*phys, spawn)) {
            return false;
        }
        voxel_renderer_.rebuild_all(*world_);
        // The tiles: the app's own load path (the generated header's
        // per-tile PNGs through the engine's memory decoder).
        tiles_ = voxel::load_tile_textures();
        return true;
    }

    void submit_and_read(const char* dump_name) {
        vkResetCommandPool(device_, pool_, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &begin);
        VkClearValue clears[2]{};
        clears[0].color = {{0.05f, 0.05f, 0.10f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = pass_;
        pass.framebuffer = framebuffer_;
        pass.renderArea.extent = {kW, kH};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(cmd_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        renderer_->flush(cmd_, 0, static_cast<float>(kW), static_cast<float>(kH));
        vkCmdEndRenderPass(cmd_);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kW, kH, 1};
        vkCmdCopyImageToBuffer(cmd_, color_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ws::gfx::handle_from_u64<VkBuffer>(readback_buf_),
                               1, &copy);
        vkEndCommandBuffer(cmd_);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        vkResetFences(device_, 1, &fence_);
        REQUIRE(vkQueueSubmit(queue_, 1, &submit, fence_) == VK_SUCCESS);
        REQUIRE(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        pixels_.assign(static_cast<const unsigned char*>(readback_mapped_),
                       static_cast<const unsigned char*>(readback_mapped_) + kW * kH * 4);
        if (const char* dir = std::getenv("WS_GPU_TEST_DUMP"); dir != nullptr && dump_name != nullptr) {
            const std::string path = std::string(dir) + "/" + dump_name + ".ppm";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", kW, kH);
                for (std::size_t i = 0; i < pixels_.size(); i += 4) {
                    std::fwrite(&pixels_[i], 1, 3, f);
                }
                std::fclose(f);
            }
        }
    }

    bool build_targets() {
        VkAttachmentDescription att[2]{};
        att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkFormat depth_format = static_cast<VkFormat>(gpu_.depth_format());
        att[1].format = depth_format;
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
        sub.pDepthStencilAttachment = &depth_ref;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = deps[0].srcStageMask;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 2;
        rp.pAttachments = att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        if (vkCreateRenderPass(device_, &rp, nullptr, &pass_) != VK_SUCCESS) {
            return false;
        }
        ws::gfx::ImageOut color{};
        if (!gpu_.create_image({kW, kH, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1},
                               color)) {
            return false;
        }
        color_ = ws::gfx::handle_from_u64<VkImage>(color.image);
        color_alloc_ = color.allocation;
        ws::gfx::ImageOut depth{};
        if (!gpu_.create_image({kW, kH, static_cast<std::uint32_t>(depth_format),
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1},
                               depth)) {
            return false;
        }
        depth_ = ws::gfx::handle_from_u64<VkImage>(depth.image);
        depth_alloc_ = depth.allocation;
        const bool has_stencil = depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
                                 depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                                 depth_format == VK_FORMAT_D16_UNORM_S8_UINT;
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.image = color_;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &color_view_) != VK_SUCCESS) {
            return false;
        }
        view.image = depth_;
        view.format = depth_format;
        const VkImageAspectFlags aspect =
            has_stencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                        : VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &depth_view_) != VK_SUCCESS) {
            return false;
        }
        const VkImageView views[2] = {color_view_, depth_view_};
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = pass_;
        fb.attachmentCount = 2;
        fb.pAttachments = views;
        fb.width = kW;
        fb.height = kH;
        fb.layers = 1;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer_) != VK_SUCCESS) {
            return false;
        }
        ws::gfx::BufferOut readback{};
        if (!gpu_.create_buffer({kW * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT},
                                readback)) {
            return false;
        }
        readback_buf_ = readback.buffer;
        readback_alloc_ = readback.allocation;
        readback_mapped_ = readback.mapped;
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = family_;
        if (vkCreateCommandPool(device_, &pool, nullptr, &pool_) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &alloc, &cmd_) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(device_, &fence, nullptr, &fence_) == VK_SUCCESS;
    }

    std::shared_ptr<ValidationCounter> counter_;
    ws::gfx::Gpu gpu_;
    std::unique_ptr<Renderer2D> renderer_;
    std::unique_ptr<voxel::World> world_;
    voxel::VoxelRenderer voxel_renderer_{};
    voxel::Player player_{};
    std::array<ws::ui::r2d::Texture, voxel::atlas::kTileCount>
        tiles_{};  // the app's own per-tile loads
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    std::uint32_t family_ = 0;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkImage color_ = VK_NULL_HANDLE, depth_ = VK_NULL_HANDLE;
    std::uint64_t color_alloc_ = 0, depth_alloc_ = 0;
    VkImageView color_view_ = VK_NULL_HANDLE, depth_view_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    std::uint64_t readback_buf_ = 0, readback_alloc_ = 0;
    void* readback_mapped_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<unsigned char> pixels_;
};

// The terrain-band column brightness profile: mean green over the rows
// that are terrain in BOTH frames (rows below max(horizon_north,
// horizon_east) - only the band guaranteed terrain in both, so the sky
// never contaminates the measure).
std::vector<double> column_profile(const std::vector<unsigned char>& px,
                                   std::uint32_t row0) {
    std::vector<double> out(kW, 0.0);
    for (std::uint32_t x = 0; x < kW; ++x) {
        double sum = 0.0;
        for (std::uint32_t y = row0; y < kH; ++y) {
            sum += px[(y * kW + x) * 4 + 1];
        }
        out[x] = sum / static_cast<double>(kH - row0);
    }
    return out;
}

// Where the north frame's features went in the east frame: the
// shift that maximizes cross-correlation of the two column profiles
// (both mean-subtracted). Positive = features moved RIGHT on screen.
int best_shift(const std::vector<double>& a, const std::vector<double>& b) {
    const double ma = std::accumulate(a.begin(), a.end(), 0.0) / a.size();
    const double mb = std::accumulate(b.begin(), b.end(), 0.0) / b.size();
    double best = -1e18;
    int best_d = 0;
    for (int d = -static_cast<int>(kW) / 2; d <= static_cast<int>(kW) / 2; ++d) {
        double c = 0.0;
        int n = 0;
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::int64_t xb = static_cast<std::int64_t>(x) + d;
            if (xb < 0 || xb >= static_cast<std::int64_t>(kW)) {
                continue;
            }
            c += (a[x] - ma) * (b[static_cast<std::uint32_t>(xb)] - mb);
            ++n;
        }
        c /= n > 0 ? n : 1;
        if (c > best) {
            best = c;
            best_d = d;
        }
    }
    return best_d;
}

}  // namespace

TEST_CASE("gpu: the app's own frame - terrain, horizon, and the turn's direction") {
    Rig rig;
    if (!rig.up) {
        MESSAGE("no Vulkan device / headless surface - skipped");
        return;
    }
    voxel::Player& player = rig.player();

    // --- frame 1: spawn pose, looking north -------------------------------
    rig.render_frame("voxel_north");

    // Terrain must exist: a healthy chunk of the frame is NOT sky (a
    // blank all-sky frame and an all-terrain frame are each a different
    // catastrophic bug; the spawn eye sits ~1.6 above ground at pitch 0,
    // so about half the frame is ground).
    std::uint32_t sky_pixels = 0;
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            if (rig.is_sky(x, y)) {
                ++sky_pixels;
            }
        }
    }
    const double sky_ratio = static_cast<double>(sky_pixels) / (kW * kH);
    MESSAGE("sky ratio: ", sky_ratio);
    CHECK(sky_ratio > 0.20);
    CHECK(sky_ratio < 0.75);

    // The horizon sits in the upper half (pitch 0, eye above ground).
    std::int64_t horizon_north = -1;
    for (std::uint32_t y = 0; y < kH; ++y) {
        if (!rig.is_sky(kW / 2, y)) {
            horizon_north = static_cast<std::int64_t>(y);
            break;
        }
    }
    REQUIRE(horizon_north >= 0);
    MESSAGE("horizon row (north): ", horizon_north, " of ", kH);
    CHECK(horizon_north < static_cast<std::int64_t>(kH) / 2);

    // The terrain is lit, earth-toned, not black, not sky.
    const Rgba under = rig.px(kW / 2, kH - 5);
    CHECK_FALSE(rig.is_sky(kW / 2, kH - 5));
    CHECK(under.r + under.g + under.b > 45);
    MESSAGE("bottom-center pixel: ", under.r, ",", under.g, ",", under.b);

    // --- frame 2: a SMALL east turn (+10 deg), the mouse path ----------------
    // A quarter-turn leaves almost no overlapping features between frames,
    // so the displacement cross-correlation is only meaningful for a small
    // turn: 10 degrees moves features by a screen-measurable amount while
    // keeping them on-screen. look_dx = radians / sensitivity, the same
    // numbers the GLFW mouse path delivers to this exact function.
    {
        ws::physics3d::World3D::Desc desc{};
        desc.gravity = {0.0f, -32.0f, 0.0f};
        desc.fixed_dt = 1.0f / 60.0f;
        desc.thread_count = 0;
        desc.max_bodies = 8;
        auto phys = ws::physics3d::World3D::create(desc);
        REQUIRE(phys.has_value());
        voxel::Player::MoveInput input;
        input.look_dx = static_cast<float>(0.1745f / 0.0025f);  // 10 deg
        player.tick(input, 1.0f / 60.0f, *phys);
        const ws::math::Vec3 look = player.look_direction();
        MESSAGE("look after 10-deg turn: ", look.x, ",", look.y, ",", look.z);
        CHECK(look.x > 0.17);   // sin(10 deg) ~ 0.1736
        CHECK(look.z < -0.98);  // still mostly north
    }
    const std::vector<unsigned char> north = rig.pixels_copy();
    rig.render_frame("voxel_east10");

    // The east frame's horizon, for the shared-terrain band.
    std::int64_t horizon_east = -1;
    for (std::uint32_t y = 0; y < kH; ++y) {
        if (!rig.is_sky(kW / 2, y)) {
            horizon_east = static_cast<std::int64_t>(y);
            break;
        }
    }
    REQUIRE(horizon_east >= 0);
    MESSAGE("horizon row (east): ", horizon_east, " of ", kH);

    // The terrain band both frames own outright: below the LOWER of the
    // two horizons... no - below the HIGHER row index (the later
    // horizon) so every row in the band is terrain in BOTH frames.
    const std::uint32_t row0 = static_cast<std::uint32_t>(
        std::max(horizon_north, horizon_east) + 8);
    const std::vector<unsigned char> east = rig.pixels_copy();
    const std::vector<double> prof_n = column_profile(north, row0);
    const std::vector<double> prof_e = column_profile(east, row0);
    const int shift = best_shift(prof_n, prof_e);
    MESSAGE("terrain displacement (east - north): ", shift, " px (positive = right)");

    // THE question: a SMALL turn right (north -> 10 deg east) must slide
    // the terrain LEFTWARD on screen. At fov 70 deg over 320 px, 10 deg of
    // yaw is ~46 px of parallax for distant features (near features move
    // more); the pin: a plainly negative displacement.
    CHECK(shift <= -20);
    CHECK(shift >= -110);

    // No validation noise while all of that happened.
    CHECK(rig.validation_messages() == 0);
}

