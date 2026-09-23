// Single translation unit that compiles VMA's implementation.
// volk must come first: with VMA_DYNAMIC_VULKAN_FUNCTIONS the allocator
// resolves every other Vulkan entry point at runtime through the two loaders
// (vkGetInstanceProcAddr / vkGetDeviceProcAddr, both volk globals) that
// app.cpp hands over via VmaAllocatorCreateInfo::pVulkanFunctions - since
// VMA 3.2 it no longer picks them up implicitly.
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

// VMA is a third-party header - keep it clear of our /W4 /WX settings.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
