#include "renderer2d.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "shaders/spirv.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>  // declarations only; the implementation TU is stb_impl.cpp
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <ws/core/log.hpp>

namespace ws::ui::r2d {

// The chunk VP rides push constants, so its size is load-bearing for the
// pipeline layout's push-constant range.
static_assert(sizeof(batch::Mat4) == 64, "push constant = exactly one Mat4");

namespace {

// TL, TR, BR, BL - matches the shared index pattern (0,1,2 / 2,3,0).
constexpr float kQuadUV[4][2] = {
    {0.0f, 0.0f},  // TL
    {1.0f, 0.0f},  // TR
    {1.0f, 1.0f},  // BR
    {0.0f, 1.0f},  // BL
};

// The white texture is the registry's index 0: Texture{} and every failed
// load resolve to it.
constexpr std::uint32_t kWhiteIndex = 0;

// Host-visible allocation for write-then-submit buffers (the index pattern,
// vertex regions, staging): sequential writes, mapped at creation. vma may
// hand back non-coherent memory, so every writer still flushes explicitly.
VmaAllocationCreateInfo host_write_alloc() {
    VmaAllocationCreateInfo info{};
    info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT;
    info.usage = VMA_MEMORY_USAGE_AUTO;
    return info;
}

VkShaderModule make_shader_module(VkDevice device, const std::uint32_t* code,
                                  std::size_t word_count) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = word_count * sizeof(std::uint32_t);
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

}  // namespace

// --- Renderer2D: shell plumbing --------------------------------------------

void Renderer2D::frame_begin(float fb_width, float fb_height) {
    fb_width_ = fb_width;
    fb_height_ = fb_height;
    batch_.new_frame();
    commands_.clear();  // capacity kept - the frame reuses the arena
    current_epoch_ = 0;
    camera_stashed_ = false;
    frame_active_ = true;
    warned_ = false;
}

void Renderer2D::frame_end() {
    frame_active_ = false;
    if (commands_.empty()) {
        return;
    }
    // Layer first (the explicit z escape hatch), then begin() block, then
    // material. Stable, because reordering must never flip two overlapping
    // same-state draws.
    std::stable_sort(commands_.begin(), commands_.end(),
                     [](const DrawCommand& a, const DrawCommand& b) {
                         if (a.sort_layer != b.sort_layer) {
                             return a.sort_layer < b.sort_layer;
                         }
                         if (a.camera_epoch != b.camera_epoch) {
                             return a.camera_epoch < b.camera_epoch;
                         }
                         return a.material_key < b.material_key;
                     });

    // Replay: a segment opens per begin() block; the block's 32-slot texture
    // table absorbs material variety, so material changes never split. Runs
    // of the same block (same camera, different layers) share the segment -
    // their sorted vertex order already paints correctly.
    std::uint32_t open_epoch = 0;  // 0 never matches a recorded block
    for (const DrawCommand& cmd : commands_) {
        if (cmd.camera_epoch != open_epoch) {
            batch_.close_segment();
            batch_.open_segment(
                batch::camera_vp(cmd.camera, fb_width_, fb_height_));
            open_epoch = cmd.camera_epoch;
        }
        float color[4];
        batch::unpack_rgba(cmd.color, color);
        if (!batch_.push_quad(cmd.pos, cmd.uv, color, cmd.texture.id)) {
            warn_caps();
        }
    }
    batch_.close_segment();
    commands_.clear();
}

void Renderer2D::frame_drop() {
    frame_active_ = false;
    camera_stashed_ = false;
    commands_.clear();
    batch_.new_frame();
}

// --- Renderer2D: GPU lifecycle ----------------------------------------------

bool Renderer2D::init(VkDevice device, VkPhysicalDevice physical_device,
                      VmaAllocator allocator, VkQueue queue,
                      uint32_t queue_family, VkRenderPass render_pass,
                      VkFormat format, uint32_t image_count) {
    device_ = device;
    physical_device_ = physical_device;
    allocator_ = allocator;
    queue_ = queue;
    queue_family_ = queue_family;
    render_pass_ = render_pass;
    format_ = format;

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

    // One binding: a chunk's whole sampler table, immutable-sampled. The
    // array size is comment-tied to kMaxTexturesPerChunk and batch.frag.
    std::array<VkSampler, batch::kMaxTexturesPerChunk> immutable{};
    immutable.fill(sampler_);
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = batch::kMaxTexturesPerChunk;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = immutable.data();
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
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

    if (!create_per_image_objects(image_count)) {
        return false;
    }
    image_count_ = image_count;
    if (!create_index_buffer() || !create_pipeline(render_pass)) {
        return false;
    }
    return true;
}

bool Renderer2D::on_swapchain_recreated(VkRenderPass render_pass,
                                        VkFormat format, uint32_t image_count) {
    render_pass_ = render_pass;
    if (pipeline_ != VK_NULL_HANDLE && format != format_) {
        // Format drift invalidates the pipeline. A same-format resize needs
        // nothing: render passes stay compatible, viewport/scissor are
        // dynamic, extents arrive per flush.
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    format_ = format;
    if (pipeline_ == VK_NULL_HANDLE && !create_pipeline(render_pass)) {
        return false;
    }
    if (image_count != image_count_) {
        destroy_per_image_objects();
        if (!create_per_image_objects(image_count)) {
            return false;
        }
        image_count_ = image_count;
    }
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
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    destroy_per_image_objects();
    if (index_buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, index_buffer_, index_allocation_);
        index_buffer_ = VK_NULL_HANDLE;
        index_allocation_ = VK_NULL_HANDLE;
    }
    for (const TextureEntry& entry : textures_) {
        vkDestroyImageView(device_, entry.view, nullptr);
        vmaDestroyImage(allocator_, entry.image, entry.allocation);
    }
    textures_.clear();
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
    allocator_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    image_count_ = 0;
}

// --- Renderer2D: GPU helpers -------------------------------------------------

bool Renderer2D::create_per_image_objects(uint32_t image_count) {
    // One set per (swapchain image, chunk slot). A set belongs to exactly
    // one image, whose fence is waited before any recording starts, so the
    // flush-time rewrite never races an in-flight command buffer - and each
    // chunk owns its set, so no set is rewritten between two binds inside
    // one command buffer.
    const uint32_t set_count = image_count * batch::kMaxChunksPerFrame;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = set_count * batch::kMaxTexturesPerChunk;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = set_count;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                               &descriptor_pool_) != VK_SUCCESS) {
        ws::core::error("ui: r2d - vkCreateDescriptorPool failed");
        return false;
    }

    const std::vector<VkDescriptorSetLayout> layouts(set_count, set_layout_);
    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = descriptor_pool_;
    set_info.descriptorSetCount = set_count;
    set_info.pSetLayouts = layouts.data();
    sets_.resize(set_count);
    if (vkAllocateDescriptorSets(device_, &set_info, sets_.data()) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d - vkAllocateDescriptorSets failed");
        sets_.clear();
        return false;
    }

    // Full-white start: flush only rewrites a chunk's used slots, but the
    // fragment shader indexes the sampler array dynamically, so every slot
    // of a bound set must hold a valid view at all times.
    std::vector<VkDescriptorImageInfo> infos(
        static_cast<std::size_t>(set_count) * batch::kMaxTexturesPerChunk);
    for (VkDescriptorImageInfo& info : infos) {
        info.sampler = sampler_;
        info.imageView = textures_[kWhiteIndex].view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    std::vector<VkWriteDescriptorSet> writes(set_count);
    for (uint32_t i = 0; i < set_count; ++i) {
        VkWriteDescriptorSet& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets_[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = batch::kMaxTexturesPerChunk;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo =
            &infos[static_cast<std::size_t>(i) * batch::kMaxTexturesPerChunk];
    }
    vkUpdateDescriptorSets(device_, set_count, writes.data(), 0, nullptr);

    // One persistently-mapped vertex region per image, sized by the static
    // frame cap so a swapchain resize never reallocates them.
    buffers_.resize(image_count);
    for (ImageBuffers& region : buffers_) {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = batch::kMaxVerticesPerFrame * sizeof(batch::Vertex);
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo alloc_info = host_write_alloc();
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info,
                            &region.buffer, &region.allocation, &mapped) !=
            VK_SUCCESS) {
            ws::core::error("ui: r2d - vertex region allocation failed");
            return false;
        }
        region.mapped = static_cast<batch::Vertex*>(mapped.pMappedData);
    }
    return true;
}

void Renderer2D::destroy_per_image_objects() {
    for (const ImageBuffers& region : buffers_) {
        vmaDestroyBuffer(allocator_, region.buffer, region.allocation);
    }
    buffers_.clear();
    sets_.clear();
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
}

bool Renderer2D::create_index_buffer() {
    // Absolute pattern: quad q's six indices are 4q + {0,1,2,2,3,0}. A chunk
    // draw with firstIndex 0 and vertexOffset = chunk.vertex_offset lands
    // each chunk-local quad q on its own vertices:
    // 4*(q0 + q) + k == 4q + vertex_offset + k.
    const uint32_t quad_capacity =
        static_cast<uint32_t>(batch::kMaxVerticesPerFrame / 4);
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size =
        static_cast<VkDeviceSize>(quad_capacity) * 6 * sizeof(uint32_t);
    buffer_info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo alloc_info = host_write_alloc();
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &index_buffer_,
                        &index_allocation_, &mapped) != VK_SUCCESS) {
        ws::core::error("ui: r2d - index buffer allocation failed");
        return false;
    }
    auto* const indices = static_cast<uint32_t*>(mapped.pMappedData);
    for (uint32_t q = 0; q < quad_capacity; ++q) {
        const uint32_t base = q * 4;
        indices[q * 6 + 0] = base + 0;  // TL
        indices[q * 6 + 1] = base + 1;  // TR
        indices[q * 6 + 2] = base + 2;  // BR
        indices[q * 6 + 3] = base + 2;  // BR
        indices[q * 6 + 4] = base + 3;  // BL
        indices[q * 6 + 5] = base + 0;  // TL
    }
    vmaFlushAllocation(allocator_, index_allocation_, 0, VK_WHOLE_SIZE);
    return true;
}

bool Renderer2D::create_pipeline(VkRenderPass render_pass) {
    VkShaderModule vert = make_shader_module(
        device_, spirv::kBatchVertSpv,
        sizeof(spirv::kBatchVertSpv) / sizeof(spirv::kBatchVertSpv[0]));
    VkShaderModule frag = make_shader_module(
        device_, spirv::kBatchFragSpv,
        sizeof(spirv::kBatchFragSpv) / sizeof(spirv::kBatchFragSpv[0]));
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vert, nullptr);  // null-tolerated
        vkDestroyShaderModule(device_, frag, nullptr);
        ws::core::error("ui: r2d - shader module creation failed");
        return false;
    }

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
    // 36-byte static_assert in batch_core.h.
    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.stride = static_cast<uint32_t>(sizeof(batch::Vertex));
    VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},         // a_pos
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8},   // a_color
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},        // a_uv
        {3, 0, VK_FORMAT_R32_SFLOAT, 32},           // a_tex_index
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

    // No depth test - 2D is pure painter's order (the sort built it).
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

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
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    const VkResult err = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);  // baked into the pipeline
    vkDestroyShaderModule(device_, frag, nullptr);
    if (err != VK_SUCCESS) {
        ws::core::error("ui: r2d - vkCreateGraphicsPipelines failed");
        return false;
    }
    return true;
}

bool Renderer2D::upload_texture(const void* pixels, uint32_t width,
                                uint32_t height, Texture* out) {
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(width) * height * 4;  // R8G8B8A8, 4 B/px

    TextureEntry entry{};
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo image_alloc{};
    image_alloc.usage = VMA_MEMORY_USAGE_AUTO;  // device-local in practice
    if (vmaCreateImage(allocator_, &image_info, &image_alloc, &entry.image,
                       &entry.allocation, nullptr) != VK_SUCCESS) {
        ws::core::error("ui: r2d - vmaCreateImage failed ({}x{})", width,
                        height);
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &entry.view) !=
        VK_SUCCESS) {
        ws::core::error("ui: r2d - vkCreateImageView failed ({}x{})", width,
                        height);
        vmaDestroyImage(allocator_, entry.image, entry.allocation);
        return false;
    }

    VkBufferCreateInfo staging_info{};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = size;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo staging_alloc = host_write_alloc();
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_allocation = VK_NULL_HANDLE;
    VmaAllocationInfo staging_mapped{};
    if (vmaCreateBuffer(allocator_, &staging_info, &staging_alloc, &staging,
                        &staging_allocation, &staging_mapped) != VK_SUCCESS) {
        ws::core::error("ui: r2d - staging buffer allocation failed");
        vkDestroyImageView(device_, entry.view, nullptr);
        vmaDestroyImage(allocator_, entry.image, entry.allocation);
        return false;
    }
    std::memcpy(staging_mapped.pMappedData, pixels,
                static_cast<std::size_t>(size));
    vmaFlushAllocation(allocator_, staging_allocation, 0, VK_WHOLE_SIZE);

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
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = entry.image;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = 1;
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
    vmaDestroyBuffer(allocator_, staging, staging_allocation);
    if (err != VK_SUCCESS) {
        ws::core::error("ui: r2d - texture upload failed (VkResult {})",
                        static_cast<int>(err));
        vkDestroyImageView(device_, entry.view, nullptr);
        vmaDestroyImage(allocator_, entry.image, entry.allocation);
        return false;
    }

    // id == registry index: [0] is white (init uploads it first), so ids
    // handed out from here start at 1.
    textures_.push_back(entry);
    if (out != nullptr) {
        *out = Texture{static_cast<uint32_t>(textures_.size() - 1)};
    }
    return true;
}

void Renderer2D::write_chunk_set(VkDescriptorSet set,
                                 const batch::Chunk& chunk) {
    if (chunk.texture_count == 0) {
        return;  // push_quad always assigns a slot, but never write 0 descriptors
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

void Renderer2D::flush(VkCommandBuffer cmd, uint32_t image_index,
                       float fb_width, float fb_height) {
    if (pipeline_ == VK_NULL_HANDLE || batch_.empty()) {
        return;
    }
    if (fb_width <= 0.0f || fb_height <= 0.0f) {
        return;  // nothing sane to draw into
    }

    // This image's fence is waited before recording started (render_frame),
    // so its vertex region and chunk sets are safely writable now.
    ImageBuffers& region = buffers_[image_index];
    const std::vector<batch::Vertex>& vertices = batch_.vertices();
    std::memcpy(region.mapped, vertices.data(),
                vertices.size() * sizeof(batch::Vertex));
    vmaFlushAllocation(allocator_, region.allocation, 0, VK_WHOLE_SIZE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &region.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT32);

    VkViewport viewport{};
    viewport.width = fb_width;
    viewport.height = fb_height;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent.width = static_cast<uint32_t>(fb_width);
    scissor.extent.height = static_cast<uint32_t>(fb_height);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    for (uint32_t c = 0; c < batch_.chunk_count(); ++c) {
        const batch::Chunk& chunk = batch_.chunk(c);
        if (chunk.quad_count == 0) {
            continue;
        }
        VkDescriptorSet set =
            sets_[image_index * batch::kMaxChunksPerFrame + c];
        write_chunk_set(set, chunk);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, static_cast<uint32_t>(sizeof(batch::Mat4)),
                           chunk.vp.m);
        // The shared absolute index pattern + vertexOffset picks out the
        // chunk's contiguous vertices; firstIndex stays 0.
        vkCmdDrawIndexed(cmd, chunk.quad_count * 6, 1, 0,
                         static_cast<int32_t>(chunk.vertex_offset), 0);
    }
}

// --- Renderer2D: guards -----------------------------------------------------

bool Renderer2D::accept(const char* what) {
    if (frame_active_) {
        return true;
    }
    warn_drop(what);
    return false;
}

void Renderer2D::warn_drop(const char* what) {
    if (warned_) {
        return;
    }
    warned_ = true;
    ws::core::warn("ui: r2d::{} dropped - draws are only valid inside "
                   "on_update(), within a begin()/end() block",
                   what);
}

void Renderer2D::warn_caps() {
    if (warned_) {
        return;
    }
    warned_ = true;
    ws::core::warn("ui: r2d draws dropped - frame batch caps exhausted "
                   "({} segments, {} vertices)",
                   batch::kMaxChunksPerFrame, batch::kMaxVerticesPerFrame);
}

void Renderer2D::warn_unknown_texture(uint32_t id) {
    if (warned_) {
        return;
    }
    warned_ = true;
    ws::core::warn("ui: r2d draw with unknown texture id {} - rendered white",
                   id);
}

// --- Renderer2D: app-facing entry points -----------------------------------

void Renderer2D::stash_camera(const Camera2D& camera) {
    ++current_epoch_;
    current_camera_ = camera;
    camera_stashed_ = true;
}

void Renderer2D::record(const char* what, const float (&pos)[4][2],
                        const float (&uv)[4][2], std::uint32_t color,
                        Texture texture) {
    if (!camera_stashed_) {
        warn_drop(what);  // outside any begin()/end() block
        return;
    }
    // Unknown ids render white: a garbage index must never reach the
    // descriptor write at flush time (it would index textures_ out of
    // bounds). Clamped here, before the material key is even packed, so a
    // garbage id cannot leak into the sort either. Only while a registry
    // exists (init() puts white at index 0 before any frame): headless
    // there is nothing to index and no flush, so synthetic ids pass
    // through untouched - the CPU-side tests rely on exactly that.
    if (!textures_.empty() && texture.id != 0 &&
        texture.id >= textures_.size()) {
        warn_unknown_texture(texture.id);
        texture = Texture{};
    }
    DrawCommand cmd;
    cmd.sort_layer = current_layer_;
    cmd.camera_epoch = current_epoch_;
    cmd.material_key = make_material_key(texture);
    cmd.camera = current_camera_;
    cmd.texture = texture;
    cmd.color = color;
    for (int i = 0; i < 4; ++i) {
        cmd.pos[i][0] = pos[i][0];
        cmd.pos[i][1] = pos[i][1];
        cmd.uv[i][0] = uv[i][0];
        cmd.uv[i][1] = uv[i][1];
    }
    commands_.push_back(cmd);
}

void Renderer2D::begin_screen() {
    if (!accept("begin")) {
        return;
    }
    // Top-left origin: center the default camera on the extent midpoint.
    const Camera2D screen{fb_width_ * 0.5f, fb_height_ * 0.5f, 0.0f, 1.0f};
    stash_camera(screen);
}

void Renderer2D::begin(const Camera2D& camera) {
    if (!accept("begin")) {
        return;
    }
    stash_camera(camera);
}

void Renderer2D::end() {
    if (!accept("end")) {
        return;
    }
    camera_stashed_ = false;  // draws after end() drop until the next begin()
}

void Renderer2D::set_layer(std::uint32_t layer) {
    if (!accept("set_layer")) {
        return;
    }
    current_layer_ = layer;
}

void Renderer2D::draw_quad(float x, float y, float w, float h,
                           std::uint32_t color, Texture texture,
                           float rotation) {
    if (!accept("draw_quad")) {
        return;
    }
    float pos[4][2];
    batch::quad_corners(x, y, w, h, rotation, pos);
    record("draw_quad", pos, kQuadUV, color, texture);
}

void Renderer2D::draw_line(float x0, float y0, float x1, float y1,
                           float thickness, std::uint32_t color) {
    if (!accept("draw_line")) {
        return;
    }
    float pos[4][2];
    float uv[4][2];
    if (!batch::line_quad(x0, y0, x1, y1, thickness, pos, uv)) {
        return;  // degenerate: live app data, not worth a warning
    }
    record("draw_line", pos, uv, color, Texture{});  // lines are solid color
}

FrameSize Renderer2D::frame_size() const {
    return FrameSize{fb_width_, fb_height_};
}

Texture Renderer2D::load_texture(const char* path) {
    Texture texture{};
    if (device_ == VK_NULL_HANDLE) {
        // No GPU context (headless tests, or a call before init()) - white,
        // not a crash.
        ws::core::warn("ui: r2d::load_texture('{}') fell back to white - no "
                       "GPU context is alive", path);
        return texture;
    }
    int width = 0;
    int height = 0;
    // Force 4 B/px: the registry is R8G8B8A8_UNORM (matches the UNORM
    // swapchain and the ImGui font atlas, so colors render as authored).
    stbi_uc* pixels = stbi_load(path, &width, &height, nullptr, STBI_rgb_alpha);
    if (pixels == nullptr) {
        ws::core::warn("ui: r2d::load_texture('{}') failed - {} (using white)",
                       path, stbi_failure_reason());
        return texture;
    }
    if (!upload_texture(pixels, static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height), &texture)) {
        ws::core::warn("ui: r2d::load_texture('{}') upload failed - using "
                       "white", path);
        texture = Texture{};
    }
    stbi_image_free(pixels);
    return texture;
}

// --- public free functions ----------------------------------------------------

namespace {

Renderer2D* g_active = nullptr;

// No app is running at all (or run() has returned): warn once per process -
// this is an app bug, not something to spam per call.
void warn_no_app(const char* what) {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    ws::core::warn("ui: r2d::{} dropped - no app is running (the 2D API only "
                   "draws while App::run() is active)", what);
}

}  // namespace

void set_active(Renderer2D* renderer) {
    g_active = renderer;
}

void begin() {
    if (g_active != nullptr) {
        g_active->begin_screen();
    } else {
        warn_no_app("begin");
    }
}

void begin(const Camera2D& camera) {
    if (g_active != nullptr) {
        g_active->begin(camera);
    } else {
        warn_no_app("begin");
    }
}

void end() {
    if (g_active != nullptr) {
        g_active->end();
    } else {
        warn_no_app("end");
    }
}

void set_layer(std::uint32_t layer) {
    if (g_active != nullptr) {
        g_active->set_layer(layer);
    } else {
        warn_no_app("set_layer");
    }
}

void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture, float rotation) {
    if (g_active != nullptr) {
        g_active->draw_quad(x, y, w, h, color, texture, rotation);
    } else {
        warn_no_app("draw_quad");
    }
}

void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color) {
    if (g_active != nullptr) {
        g_active->draw_line(x0, y0, x1, y1, thickness, color);
    } else {
        warn_no_app("draw_line");
    }
}

FrameSize frame_size() {
    if (g_active != nullptr) {
        return g_active->frame_size();
    }
    return FrameSize{};
}

Texture load_texture(const char* path) {
    if (g_active != nullptr) {
        return g_active->load_texture(path);
    }
    warn_no_app("load_texture");
    return Texture{};
}

}  // namespace ws::ui::r2d
