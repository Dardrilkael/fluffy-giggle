// The r3d mesh path's DEVICE half, end to end: a real gfx::Gpu on a
// headless surface, a real offscreen render pass, the real Renderer2D::flush
// - and the pixels read back. This is the suite that proves the Vulkan side
// (pipeline, layouts, push constants, scene block, buffers, the weave in the
// flush, the retire queue, both texture tiers) the way test_ui_mesh proves
// the CPU half. With the Khronos validation layer on, any validation
// warning or error fails the case.
//
// It needs a Vulkan device (any: a software ICD such as lavapipe is enough)
// and the VK_EXT_headless_surface instance extension, so it is opt-in
// (WS_UI_GPU_TESTS) and SKIPS - with a message, not a failure - when no
// device comes up. WS_GPU_TEST_DUMP=<dir> writes each frame as a .ppm.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "render/renderer2d.h"

#include <ws/core/log.hpp>
#include <ws/gfx/gpu.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using ws::ui::r2d::Camera3D;
using ws::ui::r2d::Renderer2D;
using ws::ui::r2d::TextureTier;
using namespace ws::ui::r3d;

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

// A 2x2 opaque pure-green PNG (generated, not a fixture file).
const unsigned char kGreenPng[] = {
#include "green_png.inc"
};

// Counts the validation layer's warnings and errors (gfx funnels them into
// ws::core as "gfx: validation: ...").
class ValidationCounter final : public ws::core::LogSink {
public:
    void write(const ws::core::LogRecord& record) override {
        if (record.level < ws::core::LogLevel::warn) {
            return;
        }
        if (record.message.find("validation requested but") !=
            std::string_view::npos) {
            layer_missing = true;  // gfx's own "running without validation"
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
    int r = 0, g = 0, b = 0, a = 0;
};

// The whole offscreen rig: device, render pass, color+depth targets,
// readback buffer, command buffer, and a Renderer2D built on it.
class Rig {
public:
    bool up = false;

    explicit Rig(bool force_chunked) {
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
        if (!force_chunked) {
            tier.bindless = gpu_.descriptor_indexing().bindless;
            tier.update_after_bind = gpu_.descriptor_indexing().update_after_bind;
        }
        bindless_ = tier.bindless;
        renderer_ = std::make_unique<Renderer2D>();
        if (!renderer_->init(device_, phys_, gpu_, queue_, family_, pass_,
                             VK_FORMAT_R8G8B8A8_UNORM, tier)) {
            return;
        }
        up = true;
    }

    ~Rig() {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }
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
    bool bindless() const { return bindless_; }
    // The layer's warnings + errors so far. A rig whose layer did not load
    // reports a loud message instead of a silently-clean zero.
    int validation_messages() const {
        if (counter_->layer_missing) {
            MESSAGE("Khronos validation layer NOT active - the zero below proves nothing");
        }
        return counter_->count;
    }
    const std::string& first_validation() const { return counter_->first; }

    // Records the renderer's frame (already recorded + frame_end'ed by the
    // caller) into a render pass and reads the pixels back.
    void render(const char* dump_name = nullptr) {
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
                               ws::gfx::handle_from_u64<VkBuffer>(readback_buf_), 1,
                               &copy);
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
        const char* dir = std::getenv("WS_GPU_TEST_DUMP");
        if (dir != nullptr && dump_name != nullptr) {
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

    Rgba px(std::uint32_t x, std::uint32_t y) const {
        const unsigned char* p = &pixels_[(y * kW + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }

private:
    bool build_targets() {
        // Render pass: RGBA8 color (-> transfer src) + the Gpu's depth format.
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
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    std::uint32_t family_ = 0;
    bool bindless_ = false;
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

constexpr Rgba kClear = {13, 13, 26, 255};  // 0.05, 0.05, 0.10

bool near_clear(const Rgba& p) {
    return std::abs(p.r - kClear.r) <= 2 && std::abs(p.g - kClear.g) <= 2 &&
           std::abs(p.b - kClear.b) <= 2;
}

// Each case runs under both texture tiers (chunked always; bindless when the
// device offers it). A missing device skips with a message.
template <class Body>
void for_each_tier(Body&& body) {
    for (const bool force_chunked : {true, false}) {
        Rig rig(force_chunked);
        if (!rig.up) {
            MESSAGE("no Vulkan device / headless surface - skipped");
            return;
        }
        if (!force_chunked && !rig.bindless()) {
            MESSAGE("device has no bindless tier - second pass skipped");
            return;
        }
        body(rig);
    }
}

}  // namespace

TEST_CASE("gpu: a lit sphere renders, top brighter than bottom, corners clear") {
    for_each_tier([](Rig& rig) {
        Renderer2D& r = rig.r();
        const auto sphere = r.create_mesh(generate_sphere(60.0f, 24, 32));
        REQUIRE(sphere);
        r.frame_begin(kW, kH);
        r.begin(Camera3D{});
        MeshMaterial material;
        material.tint = ws::ui::r2d::rgba(255, 90, 90);
        r.draw_mesh(*sphere, Transform3D{}, material);
        r.frame_end();
        rig.render(rig.bindless() ? "sphere_bindless" : "sphere_chunked");

        const Rgba top = rig.px(160, 120 - 30);
        const Rgba bottom = rig.px(160, 120 + 30);
        CHECK_FALSE(near_clear(top));
        CHECK_FALSE(near_clear(bottom));
        CHECK(top.r > bottom.r + 40);   // lit from above (world -y = up)
        CHECK(top.r > top.g + 60);      // the tint reached the shader
        CHECK(near_clear(rig.px(5, 5)));
        CHECK(near_clear(rig.px(kW - 5, kH - 5)));
        CHECK(rig.validation_messages() == 0);
    });
}

TEST_CASE("gpu: quads and a mesh interleave in (layer) order inside the pass") {
    for_each_tier([](Rig& rig) {
        Renderer2D& r = rig.r();
        const auto sphere = r.create_mesh(generate_sphere(60.0f, 24, 32));
        REQUIRE(sphere);
        r.frame_begin(kW, kH);
        r.begin(Camera3D{});  // epoch 1
        r.set_layer(3u);
        MeshMaterial material;
        material.tint = ws::ui::r2d::rgba(60, 255, 60);  // green
        r.draw_mesh(*sphere, Transform3D{}, material);
        r.begin_screen();  // epoch 2: the 2D painter, pixel space
        r.set_layer(1u);   // BEFORE the mesh: red, partly hidden by it
        r.draw_quad(80.0f, 90.0f, 160.0f, 60.0f, ws::ui::r2d::rgba(255, 0, 0));
        r.set_layer(5u);   // AFTER the mesh: blue, on top of it
        r.draw_quad(140.0f, 100.0f, 40.0f, 40.0f, ws::ui::r2d::rgba(0, 0, 255));
        r.frame_end();
        rig.render(rig.bindless() ? "order_bindless" : "order_chunked");

        const Rgba outside = rig.px(95, 120);   // in the red quad, off the sphere
        const Rgba overlap = rig.px(128, 120);  // red quad UNDER the sphere
        const Rgba on_top = rig.px(160, 120);   // blue quad OVER the sphere
        CHECK(outside.r > 200);
        CHECK(outside.g < 40);
        CHECK(overlap.g > overlap.r + 30);  // the mesh covered the red quad
        CHECK(on_top.b > 200);              // the blue quad covered the mesh
        CHECK(on_top.g < 40);
        CHECK(rig.validation_messages() == 0);
    });
}

TEST_CASE("gpu: a textured mesh samples its texture in both tiers") {
    for_each_tier([](Rig& rig) {
        Renderer2D& r = rig.r();
        const ws::ui::r2d::Texture green =
            r.load_texture_from_memory(kGreenPng, sizeof kGreenPng);
        REQUIRE(green.id != 0);
        const auto cube = r.create_mesh(generate_cube(100.0f));
        REQUIRE(cube);
        r.frame_begin(kW, kH);
        r.begin(Camera3D{});
        MeshMaterial plain;  // white tint, no texture
        Transform3D left;
        left.translation[0] = -70.0f;
        MeshMaterial textured;
        textured.texture = green;
        Transform3D right;
        right.translation[0] = 70.0f;
        r.draw_mesh(*cube, left, plain);
        r.draw_mesh(*cube, right, textured);
        r.frame_end();
        rig.render(rig.bindless() ? "texture_bindless" : "texture_chunked");

        // The default camera sees each cube's -Z face dead on, lit by the
        // ambient floor alone (the light grazes it).
        const Rgba plain_px = rig.px(160 - 85, 120);
        const Rgba green_px = rig.px(160 + 85, 120);
        CHECK(std::abs(plain_px.r - plain_px.g) < 8);  // untextured = grey
        CHECK(std::abs(plain_px.g - plain_px.b) < 8);
        CHECK(plain_px.r > 40);
        CHECK(green_px.g > green_px.r + 25);           // the texture reached it
        CHECK(green_px.g > green_px.b + 25);
        CHECK(rig.validation_messages() == 0);
    });
}

TEST_CASE("gpu: destroy retires safely; a destroyed-then-reused slot draws its new mesh") {
    for_each_tier([](Rig& rig) {
        Renderer2D& r = rig.r();
        const auto first = r.create_mesh(generate_sphere(60.0f, 16, 24));
        REQUIRE(first);
        // Frame 1: draw it, then destroy it in the SAME frame (the recorded
        // draw still holds the buffer), and reuse its slot immediately.
        r.frame_begin(kW, kH);
        r.begin(Camera3D{});
        MeshMaterial material;
        material.tint = ws::ui::r2d::rgba(255, 255, 255);
        r.draw_mesh(*first, Transform3D{}, material);
        REQUIRE(r.destroy_mesh(*first));
        const auto second = r.create_mesh(generate_cube(30.0f));
        REQUIRE(second);
        r.draw_mesh(*second, Transform3D{}, material);
        r.draw_mesh(*first, Transform3D{}, material);  // stale: rejected
        r.frame_end();
        rig.render(rig.bindless() ? "retire_bindless" : "retire_chunked");
        CHECK_FALSE(near_clear(rig.px(160, 120)));  // the sphere's draw ran
        // Several empty frames drain the retire queue; validation catches a
        // premature or double free.
        for (int i = 0; i < 6; ++i) {
            r.frame_begin(kW, kH);
            r.frame_end();
            rig.render();
        }
        CHECK(rig.validation_messages() == 0);
    });
}

TEST_CASE("gpu: many textures and layers across one frame (set growth, barriers)") {
    for_each_tier([](Rig& rig) {
        Renderer2D& r = rig.r();
        const ws::ui::r2d::Texture green =
            r.load_texture_from_memory(kGreenPng, sizeof kGreenPng);
        REQUIRE(green.id != 0);
        const auto cube = r.create_mesh(generate_cube(8.0f));
        REQUIRE(cube);
        r.frame_begin(kW, kH);
        r.begin(Camera3D{});
        // 40 meshes at distinct layers with a billboard between each pair:
        // 40 barriers, so 80+ chunks in the chunked tier - past the pool's
        // initial 16-set capacity, which must grow with the mesh sets.
        for (int i = 0; i < 40; ++i) {
            r.set_layer(static_cast<std::uint32_t>(2 * i));
            r.draw_quad(static_cast<float>(i * 3 - 60), -80.0f, 0.0f, 2.0f, 2.0f,
                        0xFFFFFFFFu);
            r.set_layer(static_cast<std::uint32_t>(2 * i + 1));
            Transform3D t;
            t.translation[0] = static_cast<float>(i * 3 - 60);
            t.translation[1] = 20.0f;
            MeshMaterial m;
            if (i % 2 == 0) {
                m.texture = green;
            }
            r.draw_mesh(*cube, t, m);
        }
        r.frame_end();
        rig.render(rig.bindless() ? "many_bindless" : "many_chunked");
        CHECK(r.stats().mesh_draws == 40u);
        CHECK(r.batch().chunk_count() >= 40u);
        CHECK(rig.validation_messages() == 0);
    });
}
