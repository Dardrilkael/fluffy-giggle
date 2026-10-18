// ws::gfx device memory + object helpers (stage 3b phase C): the ops ui's
// renderer needs from the device layer - host-write buffers, device-local
// sampled images, shader modules - promoted so ws::ui links no VMA at all.
// The renderer's arenas, upload staging and textures come through here; the
// VmaAllocator behind them never crosses into ui (the public getter's
// consumers are gfx's own TUs: this file and the swapchain's depth image).
//
// Every failure answers core::Status with the diagnostic message - gfx logs
// nothing; the consumer logs it. Guard order inside the creates: clear the
// out record, validate the descriptor (detectable with no device), check
// the device stage, then make the Vulkan/VMA call. The destroys and the
// flush are null-tolerant no-ops, so failure-path teardowns call them
// uniformly instead of carrying their own stage checks.

#include <ws/gfx/gpu.hpp>

#include <volk.h>  // meta-loader: every vk* symbol below is a volk global

// vk_mem_alloc.h is a third-party header - keep it clear of our /W4 /WX
// settings on MSVC and of the stricter clang sets on Android.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstddef>
#include <cstdint>

namespace ws::gfx {
namespace {

// This TU's error codes - the shell logs the Status it gets; nothing
// branches on the numbers. 1 is the shared lifecycle-order code; the
// 500s are this file's creates (gpu.cpp owns the 100-300s, swapchain.cpp
// the 400s).
constexpr std::int32_t kErrOrder = 1;           // create before the device stage
constexpr std::int32_t kErrInvalid = 500;       // zero-size desc / empty words
constexpr std::int32_t kErrBuffer = 501;        // vmaCreateBuffer
constexpr std::int32_t kErrImage = 502;         // vmaCreateImage
constexpr std::int32_t kErrShaderModule = 503;  // vkCreateShaderModule

}  // namespace

// --- buffers -----------------------------------------------------------------

core::Status Gpu::create_buffer(const BufferDesc& desc, BufferOut& out) const {
    out = BufferOut{};
    if (desc.size == 0) {
        return core::fail(kErrInvalid, "gfx", "buffer create with size 0");
    }
    const VmaAllocator alloc = static_cast<VmaAllocator>(allocator());
    if (alloc == nullptr) {
        return core::fail(kErrOrder, "gfx",
                          "buffer create before the device stage");
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = desc.usage;
    // The one host-write profile ui uses (vertex/index arenas, upload
    // staging): sequential writes, mapped at creation. vma may hand back
    // non-coherent memory, so every writer still flushes explicitly
    // (flush_buffer below).
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(alloc, &info, &alloc_info, &buffer, &allocation,
                        &mapped) != VK_SUCCESS) {
        return core::fail(kErrBuffer, "gfx", "vmaCreateBuffer failed");
    }
    out.buffer = buffer;
    out.allocation = allocation;
    out.mapped = mapped.pMappedData;
    return {};
}

void Gpu::destroy_buffer(void* buffer, void* allocation) const {
    const VmaAllocator alloc = static_cast<VmaAllocator>(allocator());
    // Null-tolerated (failure teardowns call uniformly); a half-empty
    // pair is nothing gfx could free through anyway.
    if (alloc == nullptr || buffer == nullptr || allocation == nullptr) {
        return;
    }
    vmaDestroyBuffer(alloc, static_cast<VkBuffer>(buffer),
                     static_cast<VmaAllocation>(allocation));
}

void Gpu::flush_buffer(void* allocation) const {
    const VmaAllocator alloc = static_cast<VmaAllocator>(allocator());
    if (alloc == nullptr || allocation == nullptr) {
        return;
    }
    // Whole-allocation: every consumer flushes everything it wrote since
    // the create or the last flush (vmaFlushAllocation reports nothing).
    vmaFlushAllocation(alloc, static_cast<VmaAllocation>(allocation), 0,
                       VK_WHOLE_SIZE);
}

// --- images ------------------------------------------------------------------

core::Status Gpu::create_image(const ImageDesc& desc, ImageOut& out) const {
    out = ImageOut{};
    if (desc.width == 0 || desc.height == 0) {
        return core::fail(kErrInvalid, "gfx", "image create with a zero extent");
    }
    const VmaAllocator alloc = static_cast<VmaAllocator>(allocator());
    if (alloc == nullptr) {
        return core::fail(kErrOrder, "gfx",
                          "image create before the device stage");
    }
    // The one image shape ui uploads: a device-local 2D sample target
    // (AUTO with no host flags lands device-local in practice). The
    // boilerplate below is every texture's - one mip, one layer, one
    // sample, optimal tiling, exclusive, starting undefined.
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = static_cast<VkFormat>(desc.format);
    info.extent.width = desc.width;
    info.extent.height = desc.height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(alloc, &info, &alloc_info, &image, &allocation,
                       nullptr) != VK_SUCCESS) {
        return core::fail(kErrImage, "gfx", "vmaCreateImage failed");
    }
    out.image = image;
    out.allocation = allocation;
    return {};
}

void Gpu::destroy_image(void* image, void* allocation) const {
    const VmaAllocator alloc = static_cast<VmaAllocator>(allocator());
    if (alloc == nullptr || image == nullptr || allocation == nullptr) {
        return;
    }
    vmaDestroyImage(alloc, static_cast<VkImage>(image),
                    static_cast<VmaAllocation>(allocation));
}

// --- shader modules ----------------------------------------------------------

core::Status Gpu::create_shader_module(const std::uint32_t* code,
                                       std::size_t word_count,
                                       void** module) const {
    if (module != nullptr) {
        *module = nullptr;  // the failure contract: cleared before anything
    }
    if (code == nullptr || word_count == 0 || module == nullptr) {
        return core::fail(kErrInvalid, "gfx",
                          "shader module from empty words");
    }
    const VkDevice vk_device = static_cast<VkDevice>(device());
    if (vk_device == nullptr) {
        return core::fail(kErrOrder, "gfx",
                          "shader module before the device stage");
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = word_count * sizeof(std::uint32_t);
    info.pCode = code;
    VkShaderModule created = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vk_device, &info, nullptr, &created) != VK_SUCCESS) {
        return core::fail(kErrShaderModule, "gfx",
                          "vkCreateShaderModule failed");
    }
    *module = created;
    return {};
}

void Gpu::destroy_shader_module(void* module) const {
    const VkDevice vk_device = static_cast<VkDevice>(device());
    if (vk_device == nullptr || module == nullptr) {
        return;
    }
    vkDestroyShaderModule(vk_device, static_cast<VkShaderModule>(module),
                          nullptr);
}

}  // namespace ws::gfx
