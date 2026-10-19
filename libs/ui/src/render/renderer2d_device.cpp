// The Vulkan device half of Renderer2D - the stage-2 split out of
// renderer2d.cpp: init/shutdown, the swapchain-facing pipeline
// lifecycle, the frame-slot arenas and descriptor tables, texture
// upload, and the flush that replays the batch inside the render pass.
// The CPU-facing half (command recording, sort/replay, the texture/
// atlas/font registries, the public free-function layer) stays in
// src/render/renderer2d.cpp. One class, two TUs: the seam is the class
// header itself (src/render/renderer2d.h), so this file adds no new
// interface - just the members the shell calls and grows.

#include "render/renderer2d.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "render/shader_provider.h"

#include <ws/core/log.hpp>

namespace ws::ui::r2d {

// The chunk VP rides push constants, so its size is load-bearing for the
// pipeline layout's push-constant range.
static_assert(sizeof(batch::Mat4) == 64, "push constant = exactly one Mat4");

namespace {

// The white texture is the registry's index 0: Texture{} and every failed
// load resolve to it.
constexpr std::uint32_t kWhiteIndex = 0;

// The chunk's clip as a scissor rect: intersected with the frame extent
// (kFullClip means "everything"), edges rounded outward by a sub-pixel so
// a boundary pixel never falls outside the intended rect. A clip that
// empties against the extent reports a zero dimension - flush skips the
// chunk, nothing legitimate could have rendered anyway.
VkRect2D chunk_scissor(const batch::Chunk& chunk, float fb_width,
                       float fb_height) {
    const float left = std::clamp(chunk.clip[0], 0.0f, fb_width);
    const float top = std::clamp(chunk.clip[1], 0.0f, fb_height);
    const float right = std::clamp(chunk.clip[0] + chunk.clip[2], 0.0f, fb_width);
    const float bottom = std::clamp(chunk.clip[1] + chunk.clip[3], 0.0f, fb_height);
    // Empty after clamping: report a zero extent BEFORE the outward rounding
    // below, which would otherwise widen e.g. [10.5, 10.5) into a 1 px
    // sliver and leak draws through a "clip everything" scope.
    if (right <= left || bottom <= top) {
        return VkRect2D{};
    }
    VkRect2D rect{};
    rect.offset.x = static_cast<int32_t>(left);
    rect.offset.y = static_cast<int32_t>(top);
    rect.extent.width = static_cast<uint32_t>(std::ceil(right)) - rect.offset.x;
    rect.extent.height = static_cast<uint32_t>(std::ceil(bottom)) - rect.offset.y;
    return rect;
}

}  // namespace

// --- Renderer2D: GPU lifecycle ----------------------------------------------

bool Renderer2D::init(VkDevice device, VkPhysicalDevice physical_device,
                      const gfx::Gpu& gpu, VkQueue queue,
                      uint32_t queue_family, VkRenderPass render_pass,
                      VkFormat format, const TextureTier& tier) {
    device_ = device;
    physical_device_ = physical_device;
    gpu_ = &gpu;
    queue_ = queue;
    queue_family_ = queue_family;
    render_pass_ = render_pass;
    format_ = format;
    tier_ = tier;
    // The tier decides which id a vertex's tex_index carries (chunk-local
    // slot vs global registry id), so the batch must know before the first
    // frame records.
    batch_.use_global_texture_ids(tier_.bindless);

    // The chunk VP rides push constants. The spec guarantees at least 128
    // bytes, so this only fires on a broken driver - fail loudly, not with
    // a pipeline layout that cannot work.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);
    if (properties.limits.maxPushConstantsSize < sizeof(batch::Mat4)) {
        ws::core::error("ui: r2d init failed - push constant limit is {} bytes",
                        properties.limits.maxPushConstantsSize);
        return false;
    }

    VkCommandPoolCreateInfo upload_pool_info{};
    upload_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    upload_pool_info.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &upload_pool_info, nullptr,
                            &upload_pool_) != VK_SUCCESS) {
        ws::core::error("ui: r2d init failed - vkCreateCommandPool");
        return false;
    }
    VkFenceCreateInfo upload_fence_info{};
    upload_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &upload_fence_info, nullptr, &upload_fence_) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d init failed - vkCreateFence");
        return false;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    // upload_texture blits a full mip chain into every texture; LINEAR
    // here picks between pre-filtered levels by LOD, so a sprite drawn
    // smaller than its bitmap samples the matching mip instead of
    // shimmering base-level texels. maxLod must open past the chain: the
    // zero-init default clamps it to 0, which would pin every sample to
    // the base level and mute the whole chain.
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d init failed - vkCreateSampler");
        return false;
    }

    // White first: every descriptor slot starts bound to it, and flat-color
    // draws and failed loads resolve to it.
    const std::uint8_t kWhitePixel[4] = {255, 255, 255, 255};
    Texture white{};
    if (!upload_texture(kWhitePixel, 1, 1, &white)) {
        ws::core::error("ui: r2d init failed - white texture upload");
        return false;
    }

    // One binding, whose shape is the tier's:
    // - bindless: the GLOBAL texture table, kMaxBindlessTextures slots
    //   (comment-tied to batch.frag's WS_BINDLESS branch and
    //   batch::kMaxBindlessTextures), partially bound (the init-time
    //   full-white write covers every roundable index; the flag keeps
    //   unbound slots legal anyway), update-after-bind when the device
    //   has it (new ids land at load time instead of each slot's idle
    //   flush). Non-immutable samplers: 1024 immutable handles would buy
    //   nothing over that init-time write, and the sampler rides every
    //   per-id write the table gets.
    // - chunked: a chunk's whole sampler table, immutable-sampled. The
    //   array size is comment-tied to kMaxTexturesPerChunk and batch.frag.
    std::array<VkSampler, batch::kMaxTexturesPerChunk> immutable{};
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
    VkDescriptorBindingFlags binding_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    if (tier_.bindless) {
        binding.descriptorCount = batch::kMaxBindlessTextures;
        if (tier_.update_after_bind) {
            binding_flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            // Sets from this layout must come from update-after-bind pools.
            set_layout_info.flags =
                VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        }
        binding_flags_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = 1;
        binding_flags_info.pBindingFlags = &binding_flags;
        set_layout_info.pNext = &binding_flags_info;
    } else {
        immutable.fill(sampler_);
        binding.descriptorCount = batch::kMaxTexturesPerChunk;
        binding.pImmutableSamplers = immutable.data();
    }
    if (vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr,
                                    &set_layout_) != VK_SUCCESS) {
        ws::core::error("ui: r2d init failed - vkCreateDescriptorSetLayout");
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.size = static_cast<uint32_t>(sizeof(batch::Mat4));
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                               &pipeline_layout_) != VK_SUCCESS) {
        ws::core::error("ui: r2d init failed - vkCreatePipelineLayout");
        return false;
    }

    // create_frame_resources builds the vertex AND index arenas, the tier's
    // descriptor tables and the per-slot chunk pools in one pass.
    if (!create_frame_resources() || !create_pipeline(render_pass)) {
        return false;
    }
    return true;
}

bool Renderer2D::on_swapchain_recreated(VkRenderPass render_pass,
                                        VkFormat format) {
    render_pass_ = render_pass;
    if (pipeline_ != VK_NULL_HANDLE && format != format_) {
        // Format drift invalidates the pipelines (both depth modes - they
        // share every other byte of state). A same-format resize needs
        // nothing: render passes stay compatible, viewport/scissor are
        // dynamic, extents arrive per flush. The per-frame-slot resources
        // (arenas, descriptor sets) are swapchain-independent and ride
        // through every rebuild untouched.
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipeline(device_, pipeline_3d_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipeline_3d_ = VK_NULL_HANDLE;
    }
    format_ = format;
    if (pipeline_ == VK_NULL_HANDLE && !create_pipeline(render_pass)) {
        return false;
    }
    return true;
}

bool Renderer2D::reload_pipelines() {
    // Dev-only rebake (WS_UI_DEV_SHADER_RELOAD caller in app.cpp). The
    // caller must first retire every in-flight frame (it waits the
    // frame-slot fences): flush() bound draws through the pipelines being
    // destroyed. Render-pass compatibility means the same render_pass_ is
    // still the right rebake input.
    // Build the replacements BEFORE touching the live pipelines: a shader
    // typo during hot reload must keep the last good pipelines rendering.
    // create_pipeline writes the members and nulls them on failure, so park
    // the old handles and restore them if the rebuild fails.
    const VkPipeline old_2d = pipeline_;
    const VkPipeline old_3d = pipeline_3d_;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_3d_ = VK_NULL_HANDLE;
    if (!create_pipeline(render_pass_)) {
        pipeline_ = old_2d;
        pipeline_3d_ = old_3d;
        ws::core::warn("ui: r2d pipeline reload failed - keeping the "
                       "previous pipelines");
        return false;
    }
    vkDestroyPipeline(device_, old_2d, nullptr);  // null-tolerated
    vkDestroyPipeline(device_, old_3d, nullptr);
    return true;
}

void Renderer2D::shutdown() {
    if (device_ == VK_NULL_HANDLE) {
        return;  // never initialized, or already shut down
    }
    vkDeviceWaitIdle(device_);
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_3d_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_3d_, nullptr);
        pipeline_3d_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    destroy_frame_resources();
    for (const TextureEntry& entry : textures_) {
        vkDestroyImageView(device_, entry.view, nullptr);
        gpu_->destroy_image(entry.image, entry.allocation);
    }
    textures_.clear();
    // Registry-derived state dies with the registry: after a re-init the old
    // page ids would resolve to rects of unrelated textures.
    atlas_pages_.clear();
    table_full_warned_ = false;
    clip_overflow_warned_ = false;
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (upload_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, upload_fence_, nullptr);
        upload_fence_ = VK_NULL_HANDLE;
    }
    if (upload_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, upload_pool_, nullptr);
        upload_pool_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    gpu_ = nullptr;
    queue_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
}

// --- Renderer2D: GPU helpers -------------------------------------------------

bool Renderer2D::create_frame_resources() {
    // Nothing here depends on the swapchain, so a resize rebuilds none of
    // it. Everything a frame slot owns is keyed by that slot, whose fence
    // is waited before any recording starts - growth and table rewrites
    // below never race an in-flight command buffer.

    // Descriptor side, per tier.
    if (tier_.bindless) {
        // One global texture table set per frame slot, from one pool. With
        // update-after-bind the table stays writable while its slot's
        // submission is still executing (that IS the feature); the pool
        // flag pairs with the layout's UPDATE_AFTER_BIND_POOL flag.
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount =
            kMaxFramesInFlight * batch::kMaxBindlessTextures;
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = kMaxFramesInFlight;
        if (tier_.update_after_bind) {
            pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        }
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                                   &table_pool_) != VK_SUCCESS) {
            ws::core::error("ui: r2d - vkCreateDescriptorPool failed");
            return false;
        }
        const std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight,
                                                         set_layout_);
        VkDescriptorSetAllocateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_info.descriptorPool = table_pool_;
        set_info.descriptorSetCount = kMaxFramesInFlight;
        set_info.pSetLayouts = layouts.data();
        table_sets_.resize(kMaxFramesInFlight);
        if (vkAllocateDescriptorSets(device_, &set_info,
                                     table_sets_.data()) != VK_SUCCESS) {
            ws::core::error("ui: r2d - vkAllocateDescriptorSets failed");
            table_sets_.clear();
            return false;
        }

        // Full-white start (the whole table, one write per slot): the
        // fragment shader indexes it dynamically, so every roundable index
        // must hold a valid view at all times - id 0 (white) covers that
        // from frame 1. Writes for ids loaded after this land through
        // pending_table_ids_ below.
        const std::size_t infos_count =
            std::size_t(kMaxFramesInFlight) * batch::kMaxBindlessTextures;
        std::vector<VkDescriptorImageInfo> infos(infos_count);
        for (VkDescriptorImageInfo& info : infos) {
            info.sampler = sampler_;
            info.imageView = textures_[kWhiteIndex].view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        std::vector<VkWriteDescriptorSet> writes(kMaxFramesInFlight);
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            VkWriteDescriptorSet& write = writes[i];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = table_sets_[i];
            write.dstBinding = 0;
            write.dstArrayElement = 0;
            write.descriptorCount = batch::kMaxBindlessTextures;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo =
                &infos[std::size_t(i) * batch::kMaxBindlessTextures];
        }
        vkUpdateDescriptorSets(device_, kMaxFramesInFlight, writes.data(), 0,
                              nullptr);

        // Id 0 is in place everywhere; anything loaded before this is
        // still pending. Nothing has been submitted yet, so every slot is
        // idle and takes its writes right now - the no-UAB path never
        // gets a safer moment than this.
        table_written_.fill(1);
        for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
            apply_pending_table_writes(slot);
        }
    } else {
        // Chunked tier: one pool + set per (frame slot, chunk slot), grown
        // by rebuild at flush when the frame needs more chunks.
        // grow_chunk_sets builds each slot's initial pool (capacity 0 ->
        // kInitialChunkCapacity) and white-inits its sets.
        chunk_sets_.resize(kMaxFramesInFlight);
        for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
            if (!grow_chunk_sets(slot, batch::kInitialChunkCapacity)) {
                return false;
            }
        }
    }

    // One persistently-mapped vertex and index arena per frame slot, both
    // sized by the INITIAL capacities - the grow helpers double them at
    // flush, the one point the slot is idle. (The index buffer used to be
    // one shared allocation; growth needs the slot-idle flush, and a shared
    // buffer would be bound by the other slot's in-flight submission.)
    arenas_.resize(kMaxFramesInFlight);
    index_arenas_.resize(kMaxFramesInFlight);
    for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
        if (!grow_vertex_arena(slot, batch::kInitialVertexCapacity) ||
            !grow_index_arena(slot, batch::kInitialVertexCapacity / 4)) {
            return false;
        }
    }
    return true;
}

void Renderer2D::destroy_frame_resources() {
    for (const FrameArena& arena : arenas_) {
        gpu_->destroy_buffer(arena.buffer, arena.allocation);
    }
    arenas_.clear();
    for (const IndexArena& arena : index_arenas_) {
        gpu_->destroy_buffer(arena.buffer, arena.allocation);
    }
    index_arenas_.clear();
    pending_table_ids_.clear();
    table_written_.fill(0);
    table_sets_.clear();
    if (table_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, table_pool_, nullptr);
        table_pool_ = VK_NULL_HANDLE;
    }
    for (ChunkSets& slot : chunk_sets_) {
        if (slot.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, slot.pool, nullptr);
            slot.pool = VK_NULL_HANDLE;
        }
        slot.sets.clear();
        slot.capacity = 0;
    }
    chunk_sets_.clear();
}

bool Renderer2D::grow_vertex_arena(uint32_t frame_slot,
                                   std::size_t min_vertices) {
    FrameArena& arena = arenas_[frame_slot];
    std::size_t capacity = arena.capacity == 0
                                ? batch::kInitialVertexCapacity
                                : arena.capacity * 2;
    while (capacity < min_vertices) {
        capacity *= 2;
    }
    // The new buffer replaces the old one wholesale: flush grows BEFORE
    // copying this frame's vertices in, and old frames' bytes are never
    // read again - no content carry-over.
    gfx::BufferOut buffer{};
    const ws::core::Status created = gpu_->create_buffer(
        {static_cast<std::uint64_t>(capacity) * sizeof(batch::Vertex),
         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT},
        buffer);
    if (!created) {
        ws::core::error("ui: r2d - vertex arena growth failed ({} -> {} "
                        "vertices, frame slot {}): {}",
                        arena.capacity, capacity, frame_slot,
                        created.error().message);
        return false;
    }
    if (arena.buffer != VK_NULL_HANDLE) {
        gpu_->destroy_buffer(arena.buffer, arena.allocation);
        ws::core::info("ui: r2d grew frame slot {}'s vertex arena {} -> {} "
                       "vertices",
                       frame_slot, arena.capacity, capacity);
    }
    arena.buffer = static_cast<VkBuffer>(buffer.buffer);
    arena.allocation = buffer.allocation;
    arena.mapped = static_cast<batch::Vertex*>(buffer.mapped);
    arena.capacity = capacity;
    return true;
}

bool Renderer2D::grow_index_arena(uint32_t frame_slot, std::size_t min_quads) {
    IndexArena& arena = index_arenas_[frame_slot];
    std::size_t capacity = arena.capacity == 0
                                ? batch::kInitialVertexCapacity / 4
                                : arena.capacity * 2;
    while (capacity < min_quads) {
        capacity *= 2;
    }
    gfx::BufferOut buffer{};
    const ws::core::Status created = gpu_->create_buffer(
        {static_cast<std::uint64_t>(capacity) * 6 * sizeof(std::uint16_t),
         VK_BUFFER_USAGE_INDEX_BUFFER_BIT},
        buffer);
    if (!created) {
        ws::core::error("ui: r2d - index arena growth failed ({} -> {} "
                        "quads, frame slot {}): {}",
                        arena.capacity, capacity, frame_slot,
                        created.error().message);
        return false;
    }
    // No refill: the flush writes each chunk's pattern every frame (the
    // indices are chunk-local u16), so a fresh buffer only needs its
    // capacity - the flush fills exactly what it draws.
    auto* const indices = static_cast<std::uint16_t*>(buffer.mapped);
    if (arena.buffer != VK_NULL_HANDLE) {
        gpu_->destroy_buffer(arena.buffer, arena.allocation);
        ws::core::info("ui: r2d grew frame slot {}'s index arena {} -> {} "
                       "quads",
                       frame_slot, arena.capacity, capacity);
    }
    arena.buffer = static_cast<VkBuffer>(buffer.buffer);
    arena.allocation = buffer.allocation;
    arena.mapped = indices;
    arena.capacity = capacity;
    return true;
}

void Renderer2D::apply_pending_table_writes(uint32_t frame_slot) {
    // Bindless tier only. Ids are dense registry indexes, so a per-slot
    // high-water mark (table_written_) is the whole state: a pending id
    // below it is already in this slot's set. Texture loads are
    // setup-granularity, so the small vectors here are fine - this never
    // runs inside a render loop.
    std::uint32_t& written = table_written_[frame_slot];
    std::vector<std::uint32_t> todo;
    for (std::uint32_t id : pending_table_ids_) {
        if (id >= written) {
            todo.push_back(id);
        }
    }
    if (todo.empty()) {
        return;
    }
    std::vector<VkDescriptorImageInfo> infos(todo.size());
    std::vector<VkWriteDescriptorSet> writes(todo.size());
    for (std::size_t i = 0; i < todo.size(); ++i) {
        infos[i].sampler = sampler_;
        infos[i].imageView = textures_[todo[i]].view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = table_sets_[frame_slot];
        write.dstBinding = 0;
        write.dstArrayElement = todo[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0,
                          nullptr);
    written = std::max(written, todo.back() + 1);
    // Drain the ids every slot has now written (below the minimum mark):
    // pending_table_ids_ only ever holds the short tail between slots.
    std::uint32_t min_written = table_written_[0];
    for (std::uint32_t mark : table_written_) {
        min_written = std::min(min_written, mark);
    }
    pending_table_ids_.erase(
        std::remove_if(pending_table_ids_.begin(), pending_table_ids_.end(),
                       [min_written](std::uint32_t id) {
                           return id < min_written;
                       }),
        pending_table_ids_.end());
}

bool Renderer2D::grow_chunk_sets(uint32_t frame_slot, uint32_t min_chunks) {
    // Chunked tier only. Grows the slot's chunk-set pool to min_chunks by
    // rebuilding it bigger: the slot's fence is waited (flush) or nothing
    // was submitted yet (init), so the old pool and its sets are
    // unreferenced. New sets start fully white - the shader indexes the
    // table dynamically, so every slot of a bound set must hold a valid
    // view at all times.
    ChunkSets& slot_sets = chunk_sets_[frame_slot];
    uint32_t capacity = slot_sets.capacity == 0
                             ? batch::kInitialChunkCapacity
                             : slot_sets.capacity * 2;
    while (capacity < min_chunks) {
        capacity *= 2;
    }
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount =
        capacity * static_cast<uint32_t>(batch::kMaxTexturesPerChunk);
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = capacity;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d - chunk-set pool growth failed ({} -> {} "
                        "sets, frame slot {})",
                        slot_sets.capacity, capacity, frame_slot);
        return false;
    }
    const std::vector<VkDescriptorSetLayout> layouts(capacity, set_layout_);
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = pool;
    set_info.descriptorSetCount = capacity;
    set_info.pSetLayouts = layouts.data();
    std::vector<VkDescriptorSet> sets(capacity);
    if (vkAllocateDescriptorSets(device_, &set_info, sets.data()) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d - chunk-set allocation failed ({} sets, "
                        "frame slot {})",
                        capacity, frame_slot);
        vkDestroyDescriptorPool(device_, pool, nullptr);
        return false;
    }
    // Full-white start for every new set, then swap only once everything
    // succeeded - a failed growth leaves the old pool bound and valid.
    const std::size_t infos_count =
        std::size_t(capacity) * batch::kMaxTexturesPerChunk;
    std::vector<VkDescriptorImageInfo> infos(infos_count);
    for (VkDescriptorImageInfo& info : infos) {
        info.sampler = sampler_;
        info.imageView = textures_[kWhiteIndex].view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    std::vector<VkWriteDescriptorSet> writes(capacity);
    for (uint32_t i = 0; i < capacity; ++i) {
        VkWriteDescriptorSet& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = batch::kMaxTexturesPerChunk;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo =
            &infos[std::size_t(i) * batch::kMaxTexturesPerChunk];
    }
    vkUpdateDescriptorSets(device_, capacity, writes.data(), 0, nullptr);
    if (slot_sets.pool != VK_NULL_HANDLE) {
        // Destroys the old pool AND its sets in one call - sets are freed
        // with their pool, never individually.
        vkDestroyDescriptorPool(device_, slot_sets.pool, nullptr);
        ws::core::info("ui: r2d grew frame slot {}'s chunk sets {} -> {}",
                       frame_slot, slot_sets.capacity, capacity);
    }
    slot_sets.pool = pool;
    slot_sets.sets = std::move(sets);
    slot_sets.capacity = capacity;
    return true;
}

bool Renderer2D::create_pipeline(VkRenderPass render_pass) {
    // The stages come from the provider: the committed spirv.h words by
    // default, the hot-reloaded set in a WS_UI_DEV_SHADER_RELOAD build.
    // The frag stage is the tier's variant - the global-table
    // (nonuniformEXT) shader or the chunked 32-slot one, fixed at init.
    const spirv::ShaderWords vert_words = spirv::vert_words();
    const spirv::ShaderWords frag_words = spirv::frag_words(tier_.bindless);
    void* vert_raw = nullptr;
    const ws::core::Status vert_module = gpu_->create_shader_module(
        vert_words.words, vert_words.count, &vert_raw);
    if (!vert_module) {
        ws::core::error("ui: r2d - vertex shader module creation failed: {}",
                        vert_module.error().message);
        return false;
    }
    void* frag_raw = nullptr;
    const ws::core::Status frag_module = gpu_->create_shader_module(
        frag_words.words, frag_words.count, &frag_raw);
    if (!frag_module) {
        gpu_->destroy_shader_module(vert_raw);  // the pair is broken
        ws::core::error("ui: r2d - fragment shader module creation failed: {}",
                        frag_module.error().message);
        return false;
    }
    const VkShaderModule vert = static_cast<VkShaderModule>(vert_raw);
    const VkShaderModule frag = static_cast<VkShaderModule>(frag_raw);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // The batch::Vertex layout, comment-tied to shaders/batch.vert and the
    // 40-byte static_assert in batch_core.h.
    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.stride = static_cast<uint32_t>(sizeof(batch::Vertex));
    VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},      // a_pos
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},  // a_color
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 28},        // a_uv
        {3, 0, VK_FORMAT_R32_SFLOAT, 36},           // a_tex_index
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor are dynamic: extents arrive per flush, so a resize
    // rebuilds nothing here.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    // Two depth modes, one pipeline each, everything else shared below:
    // - depth_off is the 2D pipeline's state, byte-identical to the
    //   single-pipeline era: test off, write off, compare ALWAYS. 2D is pure
    //   painter's order (the sort built it) and MUST never touch depth.
    // - depth_on is the 3D pipeline: LEQUAL so equal depths keep submission
    //   order (the stable sort's), depth-write on so nearer geometry
    //   occludes.
    VkPipelineDepthStencilStateCreateInfo depth_off{};
    depth_off.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_off.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineDepthStencilStateCreateInfo depth_on{};
    depth_on.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_on.depthTestEnable = VK_TRUE;
    depth_on.depthWriteEnable = VK_TRUE;
    depth_on.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.minSampleShading = 1.0f;

    // Classic src-alpha blend; the alpha channel itself composites over
    // ONE_MINUS_SRC_ALPHA, the same attachment blend the ImGui backend uses.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                      VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    // One create info, two depth states: the pipelines share everything
    // (shaders, layout, render pass) - only the depth mode differs, so the
    // swap below is the whole difference between them.
    pipeline_info.pDepthStencilState = &depth_off;
    const VkResult err_2d = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
    pipeline_info.pDepthStencilState = &depth_on;
    const VkResult err_3d = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_3d_);
    gpu_->destroy_shader_module(vert);  // baked into the pipelines
    gpu_->destroy_shader_module(frag);
    if (err_2d != VK_SUCCESS || err_3d != VK_SUCCESS) {
        // Both in lockstep or neither: the callers (shutdown, the drift
        // path) tolerate nulls, and flush checks one - they must agree.
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipeline(device_, pipeline_3d_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
        pipeline_3d_ = VK_NULL_HANDLE;
        ws::core::error(
            "ui: r2d - vkCreateGraphicsPipelines failed ({}, {})",
            static_cast<int>(err_2d), static_cast<int>(err_3d));
        return false;
    }
    return true;
}

bool Renderer2D::upload_texture(const void* pixels, uint32_t width,
                                uint32_t height, Texture* out) {
    // Bindless tier: ids index the shader's fixed 1024-slot table, so the
    // registry cannot outgrow it. Refuse the load rather than hand out an
    // id the table cannot hold - the caller's failed-load contract is
    // white (id 0), so new textures degrade, never break. The chunked
    // tier has no such limit: its tables are per-chunk and grow with the
    // chunks.
    if (tier_.bindless && textures_.size() >= batch::kMaxBindlessTextures) {
        if (!table_full_warned_) {
            table_full_warned_ = true;
            ws::core::warn("ui: r2d texture table full ({} textures) - "
                           "further loads render white",
                           batch::kMaxBindlessTextures);
        }
        return false;
    }

    const VkDeviceSize size =
        static_cast<VkDeviceSize>(width) * height * 4;  // R8G8B8A8, 4 B/px

    // The full mip chain: floor(log2(max(w, h))) + 1 levels - bit_width
    // IS that, for integers (2048 -> 12, 3x5 -> 3, 1x1 -> 1). Level 0
    // takes the upload copy; each later level is blitted down from the
    // one before it, so a sprite drawn smaller than its bitmap samples
    // a pre-filtered mip. The 1x1 white lands at exactly 1 level and
    // skips the cascade.
    const uint32_t mip_levels = std::bit_width(std::max(width, height));

    TextureEntry entry{};
    gfx::ImageOut image{};
    const ws::core::Status image_created = gpu_->create_image(
        {width, height, VK_FORMAT_R8G8B8A8_UNORM,
         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT,
         mip_levels},
        image);
    if (!image_created) {
        ws::core::error("ui: r2d - texture image create failed ({}x{}): {}",
                        width, height, image_created.error().message);
        return false;
    }
    entry.image = static_cast<VkImage>(image.image);
    entry.allocation = image.allocation;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = mip_levels;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &entry.view) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d - vkCreateImageView failed ({}x{})", width,
                        height);
        gpu_->destroy_image(entry.image, entry.allocation);
        return false;
    }

    gfx::BufferOut staging_out{};
    const ws::core::Status staging_created = gpu_->create_buffer(
        {static_cast<std::uint64_t>(size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
        staging_out);
    if (!staging_created) {
        ws::core::error("ui: r2d - staging buffer allocation failed: {}",
                        staging_created.error().message);
        vkDestroyImageView(device_, entry.view, nullptr);
        gpu_->destroy_image(entry.image, entry.allocation);
        return false;
    }
    const VkBuffer staging = static_cast<VkBuffer>(staging_out.buffer);
    std::memcpy(staging_out.mapped, pixels,
                static_cast<std::size_t>(size));
    gpu_->flush_buffer(staging_out.allocation);

    // One-off submit on the graphics queue - textures load at load
    // granularity, so the synchronous fence wait is fine.
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = upload_pool_;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult err = vkAllocateCommandBuffers(device_, &cmd_info, &cmd);
    if (err == VK_SUCCESS) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(cmd, &begin_info);
    }
    if (err == VK_SUCCESS) {
        // The whole chain starts life undefined, so the head barrier
        // covers every level; the copy writes level 0 and the cascade
        // below fills the rest.
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = entry.image;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = mip_levels;
        to_dst.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_dst);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, staging, entry.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);

        // The cascade: level `level` blits down into level+1. A source
        // level flips to transfer-src for the blit that reads it, then
        // retires straight to sampled - it is done, nothing touches it
        // again. The LAST level never sources a blit: it stays a
        // transfer target and rides to_sampled below, which therefore
        // covers exactly one level either way - at 1 level the plain
        // single-mip path, unchanged.
        for (uint32_t level = 0; level + 1 < mip_levels; ++level) {
            VkImageMemoryBarrier to_src{};
            to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_src.image = entry.image;
            to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_src.subresourceRange.baseMipLevel = level;
            to_src.subresourceRange.levelCount = 1;
            to_src.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &to_src);

            // Halve each axis per level, floored at 1: a non-square
            // image keeps producing real extents down to the 1x1 tail.
            const uint32_t src_w = std::max(1u, width >> level);
            const uint32_t src_h = std::max(1u, height >> level);
            const uint32_t dst_w = std::max(1u, width >> (level + 1));
            const uint32_t dst_h = std::max(1u, height >> (level + 1));
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level;
            blit.srcSubresource.layerCount = 1;
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level + 1;
            blit.dstSubresource.layerCount = 1;
            blit.srcOffsets[1].x = static_cast<int32_t>(src_w);
            blit.srcOffsets[1].y = static_cast<int32_t>(src_h);
            blit.srcOffsets[1].z = 1;
            blit.dstOffsets[1].x = static_cast<int32_t>(dst_w);
            blit.dstOffsets[1].y = static_cast<int32_t>(dst_h);
            blit.dstOffsets[1].z = 1;
            vkCmdBlitImage(cmd, entry.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);

            VkImageMemoryBarrier retired{};
            retired.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            retired.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            retired.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            retired.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            retired.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            retired.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            retired.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            retired.image = entry.image;
            retired.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            retired.subresourceRange.baseMipLevel = level;
            retired.subresourceRange.levelCount = 1;
            retired.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &retired);
        }

        VkImageMemoryBarrier to_sampled{};
        to_sampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_sampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_sampled.image = entry.image;
        to_sampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_sampled.subresourceRange.baseMipLevel = mip_levels - 1;
        to_sampled.subresourceRange.levelCount = 1;
        to_sampled.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_sampled);
        err = vkEndCommandBuffer(cmd);
    }
    if (err == VK_SUCCESS) {
        vkResetFences(device_, 1, &upload_fence_);  // from the previous upload
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        err = vkQueueSubmit(queue_, 1, &submit, upload_fence_);
    }
    if (err == VK_SUCCESS) {
        err = vkWaitForFences(device_, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
    }
    if (cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, upload_pool_, 1, &cmd);
    }
    gpu_->destroy_buffer(staging_out.buffer, staging_out.allocation);
    if (err != VK_SUCCESS) {
        ws::core::error("ui: r2d - texture upload failed (VkResult {})",
                        static_cast<int>(err));
        vkDestroyImageView(device_, entry.view, nullptr);
        gpu_->destroy_image(entry.image, entry.allocation);
        return false;
    }

    // id == registry index: [0] is white (init uploads it first), so ids
    // handed out from here start at 1.
    textures_.push_back(entry);
    const uint32_t id = static_cast<uint32_t>(textures_.size() - 1);
    if (out != nullptr) {
        *out = Texture{id};
    }
    // Bindless tier: queue the table write. With update-after-bind it can
    // land in every slot right now, even mid-frame (that IS the feature);
    // without it each slot takes its writes at its idle flush, so a load
    // mid-run renders white for at most a couple of frames per slot.
    // Before the tables exist (init's own white upload), the pending
    // queue is the only option - create_frame_resources drains it once
    // they are up.
    if (tier_.bindless) {
        pending_table_ids_.push_back(id);
        if (tier_.update_after_bind && !table_sets_.empty()) {
            for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) {
                apply_pending_table_writes(slot);
            }
        }
    }
    return true;
}

void Renderer2D::write_chunk_set(VkDescriptorSet set,
                                 const batch::Chunk& chunk) {
    if (chunk.texture_count == 0) {
        // A rounded-only chunk never assigns a slot (push_rounded_quad
        // takes none) - and never write 0 descriptors.
        return;
    }
    VkDescriptorImageInfo infos[batch::kMaxTexturesPerChunk];
    for (uint32_t i = 0; i < chunk.texture_count; ++i) {
        infos[i].sampler = sampler_;
        infos[i].imageView = textures_[chunk.texture_ids[i]].view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = chunk.texture_count;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = infos;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void Renderer2D::flush(VkCommandBuffer cmd, uint32_t frame_slot,
                       float fb_width, float fb_height) {
    if (pipeline_ == VK_NULL_HANDLE || batch_.empty()) {
        return;
    }
    if (fb_width <= 0.0f || fb_height <= 0.0f) {
        return;  // nothing sane to draw into
    }

    // This frame slot's fence is waited before recording started
    // (render_frame), so its arenas, tables and chunk sets are safely
    // writable here - and grown here, when the frame outgrew them. Growth
    // failure keeps the old (smaller) buffer and drops the frame's draws:
    // never a silent partial memcpy, the error log already fired.
    FrameArena& arena = arenas_[frame_slot];
    const std::vector<batch::Vertex>& vertices = batch_.vertices();
    if (vertices.size() > arena.capacity) {
        if (!grow_vertex_arena(frame_slot, vertices.size())) {
            return;
        }
    }
    std::memcpy(arena.mapped, vertices.data(),
                vertices.size() * sizeof(batch::Vertex));
    // vma may hand back non-coherent memory: make the arena's writes
    // visible before the submit.
    gpu_->flush_buffer(arena.allocation);

    // A full vertex arena is exactly capacity/4 quads; the index arena
    // grows in step so every chunk's pattern instance fits.
    IndexArena& indices = index_arenas_[frame_slot];
    const std::size_t quads = vertices.size() / 4;
    if (quads > indices.capacity) {
        if (!grow_index_arena(frame_slot, quads)) {
            return;
        }
    }

    // Chunk-local u16 pattern, written fresh every frame: quad q of chunk
    // c -> 4q + {0,1,2,2,3,0} at the chunk's own base. Chunks are
    // contiguous (start_chunk stamps the current vertex count), so
    // (vertex_offset / 4) * 6 is the chunk's first index and the write
    // regions never overlap. Per chunk, never one global fill: a
    // texture-table split can start a chunk at any quad index, so the
    // LOCAL quad number restarts per chunk - that is what keeps every
    // index inside the u16 space (kMaxQuadsPerChunk bounds quad_count).
    for (uint32_t c = 0; c < batch_.chunk_count(); ++c) {
        const batch::Chunk& chunk = batch_.chunk(c);
        std::uint16_t* dst = indices.mapped +
            (static_cast<std::size_t>(chunk.vertex_offset) / 4) * 6;
        for (uint32_t q = 0; q < chunk.quad_count; ++q) {
            const std::uint16_t base = static_cast<std::uint16_t>(q * 4);
            dst[q * 6 + 0] = base + 0;  // TL
            dst[q * 6 + 1] = base + 1;  // TR
            dst[q * 6 + 2] = base + 2;  // BR
            dst[q * 6 + 3] = base + 2;  // BR
            dst[q * 6 + 4] = base + 3;  // BL
            dst[q * 6 + 5] = base + 0;  // TL
        }
    }
    // Same non-coherent visibility contract as the vertex arena above.
    gpu_->flush_buffer(indices.allocation);

    // Vertex/index binds and the viewport are pipeline-independent state -
    // bound once; the pipeline itself binds per chunk below (bind on change).
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &arena.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, indices.buffer, 0, VK_INDEX_TYPE_UINT16);

    VkViewport viewport{};
    viewport.width = fb_width;
    viewport.height = fb_height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Descriptor side, per tier - both are idle right now. Bindless: this
    // slot takes any pending table writes, then the ONE global table set
    // binds for the whole frame (tex_index already carries global ids).
    // Chunked: grow the slot's chunk sets first so every chunk below has
    // its own set - a mid-loop growth would leave a partial frame.
    if (tier_.bindless) {
        apply_pending_table_writes(frame_slot);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1,
                                &table_sets_[frame_slot], 0, nullptr);
    } else if (batch_.chunk_count() > chunk_sets_[frame_slot].capacity) {
        if (!grow_chunk_sets(frame_slot, batch_.chunk_count())) {
            return;
        }
    }

    VkPipeline bound = VK_NULL_HANDLE;  // nothing bound when recording starts
    for (uint32_t c = 0; c < batch_.chunk_count(); ++c) {
        const batch::Chunk& chunk = batch_.chunk(c);
        if (chunk.quad_count == 0) {
            continue;
        }
        // One chunk = one scissor: each chunk's clip (intersected with the
        // frame extent) applies before its draw. A clip that empties out
        // skips the chunk - its draws had nowhere to land.
        const VkRect2D scissor = chunk_scissor(chunk, fb_width, fb_height);
        if (scissor.extent.width == 0 || scissor.extent.height == 0) {
            continue;
        }
        // One chunk = one pipeline: the depth-tested 3D pipeline for a
        // perspective block's chunks, the 2D painter's one otherwise. Bound
        // on change only, after the skips - a skipped chunk never binds.
        const VkPipeline want = chunk.depth_test ? pipeline_3d_ : pipeline_;
        if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
        }
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (!tier_.bindless) {
            // Chunked tier: the chunk's own 32-slot table, rewritten only
            // after this slot's fence was waited and strictly before its
            // own bind.
            VkDescriptorSet set = chunk_sets_[frame_slot].sets[c];
            write_chunk_set(set, chunk);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout_, 0, 1, &set, 0, nullptr);
        }
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, static_cast<uint32_t>(sizeof(batch::Mat4)),
                           chunk.vp.m);
        // Chunk-local indices: firstIndex lands on this chunk's pattern
        // instance; vertexOffset rebases it onto the chunk's contiguous
        // vertices (4*(local q) + vertex_offset + k == the global quad).
        const uint32_t first_index = (chunk.vertex_offset / 4) * 6;
        vkCmdDrawIndexed(cmd, chunk.quad_count * 6, 1, first_index,
                         static_cast<int32_t>(chunk.vertex_offset), 0);
    }
}

}  // namespace ws::ui::r2d
