// ws::gfx::Gpu - the Vulkan device layer (stage 3b of the ui split): the
// loader, the instance (+ optional validation), the surface, physical-device
// + queue selection, the device and the VMA allocator. Promoted out of
// ws::ui's app shell; ws::ui stays the only consumer - apps never include
// this lib.
//
// All Vulkan/VMA types live in this TU behind the pimpl (include/ is
// ws-spelled only; handles cross as opaque void* and the consumer casts
// them at its own single unwrap point). Every failure returns core::Status
// with the diagnostic message - the shell logs it; the two startup info
// lines (GPU name, texture tier) log directly, diagnostics not failures.
//
// The stages keep the shell's Android-surface-loss contract: init() runs
// once per process (context stage), attach_surface()/detach_surface() ride
// the window, and the FIRST attach additionally selects the physical device
// and creates the device + queue + allocator (the device stage) - present
// support is checked against the surface, so the device stage cannot come
// up before one exists, and it then survives every detach.

#include <ws/gfx/gpu.hpp>

#include <ws/core/log.hpp>

#include "gpu_choice.h"

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

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace ws::gfx {
namespace {

// gfx's internal error codes - the shell logs the Status it gets; nothing
// branches on the numbers. Grouped by stage: lifecycle misuse, the context
// stage, the surface, the device stage.
constexpr std::int32_t kErrOrder = 1;      // attach before init, double init, ...
constexpr std::int32_t kErrLoader = 100;  // volkInitialize
constexpr std::int32_t kErrInstance = 101;  // vkCreateInstance
constexpr std::int32_t kErrSurface = 200;  // platform surface creation
constexpr std::int32_t kErrNoDevice = 300;  // no qualifying physical device
constexpr std::int32_t kErrDevice = 301;    // vkCreateDevice
constexpr std::int32_t kErrAllocator = 302;  // VMA

// The debug-utils callback funnelling the validation layer's messages into
// ws::core's log (the layer itself is requested in create_instance when
// DeviceOptions::enable_validation asks). Errors and warnings keep their
// own level so a synchronization violation is impossible to miss; the
// layer's own INFO banner reports which checks (sync validation included)
// are active.
VKAPI_ATTR VkBool32 VKAPI_CALL validation_log(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    (void)types;
    (void)user_data;
    const char* message = data->pMessage ? data->pMessage : "<no message>";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        core::error("gfx: validation: {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) !=
               0) {
        core::warn("gfx: validation: {}", message);
    } else {
        core::info("gfx: validation: {}", message);
    }
    return VK_FALSE;
}

// pick_depth_format's predicate (gpu_choice.h): the optimal-tiling
// format-properties query, so the walk itself stays testable headless.
bool supports_depth_attach(uint32_t format, void* user) {
    const auto physical = static_cast<VkPhysicalDevice>(user);
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, static_cast<VkFormat>(format),
                                        &properties);
    return (properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

// One of a device's qualifications (select_device): the swapchain
// extension, without which it can never present.
bool device_has_swapchain(VkPhysicalDevice candidate) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, available.data());
    for (const VkExtensionProperties& extension : available) {
        if (std::string_view(extension.extensionName) ==
            VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
            return true;
        }
    }
    return false;
}

}  // namespace

struct Gpu::Impl {
    // The platform seam, as handed over at init(): the extension-name
    // POINTERS are consumed once (copied into `extensions` below - the
    // strings themselves are platform-owned and must outlive the Gpu),
    // while create_surface + user run at every attach_surface().
    SurfaceSource source{};
    std::vector<const char*> extensions;
    DeviceOptions options{};

    bool instance_up = false;
    bool device_stage_up = false;

    VkInstance instance = VK_NULL_HANDLE;
    // The instance version the loader supports (M9): the bindless tier's
    // shader is SPIR-V 1.3 (nonuniformEXT), so its device needs Vulkan
    // 1.1+, which needs a 1.1+ instance. create_instance requests exactly
    // this; the tier probe (create_device) only runs when it reached 1.1,
    // and a 1.0-only loader means the chunked fallback tier - never a
    // failure.
    uint32_t instance_api_version = VK_API_VERSION_1_0;
    // The validation layer's message funnel into ws::core via
    // validation_log. Destroyed before the instance itself.
    VkDebugUtilsMessengerEXT validation_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;
    // VkFormat, numeric (gpu_choice.h's spelling): the depth-attachment
    // format chosen once at device selection - every render pass the
    // process builds must use it.
    uint32_t depth_format = 0;
    DescriptorIndexing indexing{};

    core::Status create_instance();
    core::Status select_device();
    core::Status create_device();
    core::Status create_allocator();

    void destroy_surface();
    void destroy_device_stage();
    void destroy_instance_stage();
};

core::Status Gpu::Impl::create_instance() {
    const VkResult err = volkInitialize();
    if (err != VK_SUCCESS) {
        return core::fail(kErrLoader, "gfx",
                          std::format("Vulkan loader not found (volkInitialize = "
                                      "VkResult {})",
                                      static_cast<int>(err)));
    }

    // WSI extensions per platform: whatever the consumer's host needs to
    // build its surface (the windowing lib's required set on desktop,
    // surface + android_surface on Android). Copied at init(); the opt-in
    // validation path below may append one more.
    std::vector<const char*> enabled_extensions = extensions;
    std::vector<const char*> layers;

    // Opt-in validation (DeviceOptions::enable_validation - the shell's
    // WS_UI_VALIDATION env probe reads the var; this lib only takes the
    // answer): the Khronos validation layer plus a debug-utils messenger
    // logging through ws::core. Off by default - validation costs frames
    // and an SDK must be installed. Synchronization validation is enabled
    // by default in every SDK layer >= 1.3.239, and the layer's own startup
    // banner (an INFO message) reports whether it is active - so the log
    // itself shows what ran. A machine without the layer warns once and
    // runs without it; never a requirement.
    bool validation_on = false;
    if (options.enable_validation) {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> available_layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
        bool has_khronos = false;
        for (const VkLayerProperties& layer : available_layers) {
            if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_VALIDATION") {
                has_khronos = true;
                break;
            }
        }
        uint32_t extension_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> available_extensions(extension_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &extension_count,
                                               available_extensions.data());
        bool has_debug_utils = false;
        for (const VkExtensionProperties& extension : available_extensions) {
            if (std::string_view(extension.extensionName) ==
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
                has_debug_utils = true;
                break;
            }
        }
        if (has_khronos && has_debug_utils) {
            layers.push_back("VK_LAYER_KHRONOS_VALIDATION");
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validation_on = true;
        } else {
            core::warn("gfx: validation requested but the Khronos validation "
                       "layer or debug utils is not installed (needs the "
                       "Vulkan SDK) - running without validation");
        }
    }

    // Instance version probe (M9): ask the LOADER (not any device) for
    // the newest Vulkan version it can instantiate, and request exactly
    // that. gfx's own needs are WSI basics (1.0 everywhere), but the
    // bindless texture tier's device probe runs over 1.1+ core entry
    // points (vkGetPhysicalDeviceFeatures2) and its shader is SPIR-V
    // 1.3 - a 1.0-only loader keeps the lowest version, which pins the
    // renderer to the chunked fallback tier. Requesting the loader's own
    // version never fails on an older driver: the apiVersion is a
    // maximum, not a requirement.
    instance_api_version = VK_API_VERSION_1_0;
    const auto enumerate_instance_version =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version != nullptr) {
        if (enumerate_instance_version(&instance_api_version) == VK_SUCCESS) {
            // Unusable loader (should not happen, but a zeroed or bogus
            // version must not reach VkApplicationInfo).
            if (instance_api_version == 0) {
                instance_api_version = VK_API_VERSION_1_0;
            }
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // Read once, right here: the driver copies the string during
    // vkCreateInstance, so the caller's buffer does not need to outlive
    // init() - only the call.
    app_info.pApplicationName =
        options.application_name != nullptr ? options.application_name : "ws";
    app_info.apiVersion = instance_api_version;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();
    create_info.enabledExtensionCount =
        static_cast<uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
    if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
        return core::fail(kErrInstance, "gfx", "vkCreateInstance failed");
    }
    volkLoadInstance(instance);

    if (validation_on) {
        // Messenger, not just the layer: without it the layer's findings
        // go to the OS debug output, not our log. INFO severity included
        // so the layer's startup banner (its active checks, sync
        // validation among them) lands in the log too.
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType =
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &validation_log;
        if (vkCreateDebugUtilsMessengerEXT(instance, &messenger_info, nullptr,
                                           &validation_messenger) == VK_SUCCESS) {
            core::info("gfx: validation layers enabled");
        } else {
            validation_messenger = VK_NULL_HANDLE;
            core::warn("gfx: vkCreateDebugUtilsMessengerEXT failed - the layer "
                       "is on but its messages go to the OS debug output, not "
                       "this log");
        }
    }
    return {};
}

core::Status Gpu::Impl::select_device() {
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS ||
        device_count == 0) {
        return core::fail(kErrNoDevice, "gfx", "no Vulkan-capable devices found");
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    // A device qualifies when it has a queue family that can both draw and
    // present on our surface, plus the swapchain extension. One family
    // doing both is true for essentially every setup.
    struct Candidate {
        VkPhysicalDevice device;
        uint32_t family;
    };
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice physical : devices) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count,
                                                 families.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &can_present);
            if (can_present != VK_TRUE || !device_has_swapchain(physical)) {
                continue;
            }
            // The consumer's batch shader indexes its sampler array with a
            // non-constant index, which the spec gates behind
            // shaderSampledImageArrayDynamicIndexing; skip a device that
            // lacks it rather than fail later in create_device() when
            // another candidate might have it.
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(physical, &features);
            if (!features.shaderSampledImageArrayDynamicIndexing) {
                continue;
            }
            candidates.push_back({physical, i});
            break;
        }
    }
    if (candidates.empty()) {
        return core::fail(kErrNoDevice, "gfx",
                          "no device can present to the window surface and "
                          "supports shaderSampledImageArrayDynamicIndexing");
    }

    // Prefer a discrete GPU, fall back to the first qualifier.
    const Candidate* chosen = &candidates[0];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(chosen->device, &properties);
    for (const Candidate& candidate : candidates) {
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate.device, &candidate_properties);
        if (candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = &candidate;
            properties = candidate_properties;
            break;
        }
    }

    physical_device = chosen->device;
    queue_family = chosen->family;
    // Depth format for every swapchain this process will build, chosen
    // here because select_device runs exactly once and strictly before
    // the consumer builds any render pass - that ordering IS the
    // render-pass-compatibility guarantee (no pass ever exists with
    // another format). D32 -> D24S8 -> D16, first with
    // DEPTH_STENCIL_ATTACHMENT support in optimal tiling (the walk is
    // pick_depth_format, gpu_choice.h; this is its query). A GPU with no
    // depth-attachable format cannot run a depth-tested render pass -
    // fail loudly, same rule as the dynamic-indexing feature above.
    const uint32_t preferred[] = {VK_FORMAT_D32_SFLOAT,
                                  VK_FORMAT_D24_UNORM_S8_UINT,
                                  VK_FORMAT_D16_UNORM};
    depth_format = pick_depth_format(
        preferred, sizeof(preferred) / sizeof(preferred[0]),
        &supports_depth_attach, physical_device);
    if (depth_format == 0) {
        return core::fail(kErrNoDevice, "gfx",
                          "device supports no depth-attachable format");
    }
    core::info("gfx: using GPU \"{}\"", properties.deviceName);
    return {};
}

core::Status Gpu::Impl::create_device() {
    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    // The consumer's batch shader indexes its texture array with a
    // non-constant (runtime) index - both render tiers' variants do -
    // which the spec gates behind this feature; without it, dynamic
    // sampler-array indexing is undefined behavior even where a given
    // driver happens to tolerate it. Required, not optional: fail loudly
    // rather than ship a renderer that silently depends on driver
    // leniency.
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(physical_device, &available);
    if (!available.shaderSampledImageArrayDynamicIndexing) {
        return core::fail(kErrDevice, "gfx",
                          "GPU lacks shaderSampledImageArrayDynamicIndexing, "
                          "required by the batch shader's texture array");
    }
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

    // M9: the descriptor-indexing probe (the texture tier), parameterized
    // by options.want_bindless + options.bindless_descriptor_limit - the
    // decision itself is decide_indexing() (gpu_choice.h), tested
    // headless; this body only gathers what the device advertised. The
    // bindless tier needs descriptor indexing - partially-bound tables,
    // nonuniform indexing in the shader, and (ideally) update-after-bind
    // table writes. Order matters for old drivers: the plain 1.0 property
    // query and the extension list run FIRST, and the 1.1+ pNext-chained
    // probes only touch devices that already claimed the dialect (core
    // 1.2 or VK_EXT_descriptor_indexing) - a pre-1.2 device never meets
    // an unknown pNext struct. Any miss (1.0 loader or device, missing
    // feature or limit, bindless not requested) falls back to the chunked
    // tier: never a failure, the spec's safe-fallback path.
    indexing = DescriptorIndexing{};  // chunked is the default
    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features enable_12{};
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT enable_ext{};
    void* enable_chain = nullptr;  // VkDeviceCreateInfo pNext
    IndexingCaps caps{};
    bool core_12 = false;
    std::string tier_miss = "bindless was not requested";
    if (options.want_bindless && options.bindless_descriptor_limit > 0) {
        tier_miss = "the Vulkan loader or device is pre-1.1";
        caps.loader_1_1 = instance_api_version >= VK_API_VERSION_1_1;
        if (caps.loader_1_1) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical_device, &props);
            core_12 = props.apiVersion >= VK_API_VERSION_1_2;
            bool has_di_ext = false;
            if (!core_12) {
                // 1.1 device: descriptor indexing only via the extension.
                uint32_t ext_count = 0;
                vkEnumerateDeviceExtensionProperties(physical_device, nullptr,
                                                     &ext_count, nullptr);
                std::vector<VkExtensionProperties> advertised_extensions(ext_count);
                vkEnumerateDeviceExtensionProperties(
                    physical_device, nullptr, &ext_count,
                    advertised_extensions.data());
                for (const VkExtensionProperties& ext : advertised_extensions) {
                    if (std::string_view(ext.extensionName) ==
                        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) {
                        has_di_ext = true;
                        break;
                    }
                }
            }
            tier_miss = "descriptor indexing is not advertised";
            if (core_12 || has_di_ext) {
                caps.dialect = true;
                // Features: the dialect's struct - same field names either
                // way; the 1.2 one adds the descriptorIndexing aggregate
                // switch and unrelated 1.2 features. The update-after-bind
                // limits ride the properties pNext chain.
                VkPhysicalDeviceDescriptorIndexingPropertiesEXT di_props{};
                di_props.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 props2{};
                props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                props2.pNext = &di_props;
                vkGetPhysicalDeviceProperties2(physical_device, &props2);

                bool has_nonuniform = false;
                bool has_partial = false;
                bool has_uab = false;
                if (core_12) {
                    VkPhysicalDeviceVulkan12Features probe12{};
                    probe12.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                    VkPhysicalDeviceFeatures2 features2{};
                    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    features2.pNext = &probe12;
                    vkGetPhysicalDeviceFeatures2(physical_device, &features2);
                    has_nonuniform =
                        probe12.shaderSampledImageArrayNonUniformIndexing ==
                        VK_TRUE;
                    has_partial =
                        probe12.descriptorBindingPartiallyBound == VK_TRUE;
                    has_uab =
                        probe12.descriptorBindingSampledImageUpdateAfterBind ==
                        VK_TRUE;
                } else {
                    VkPhysicalDeviceDescriptorIndexingFeaturesEXT probe_ext{};
                    probe_ext.sType =
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
                    VkPhysicalDeviceFeatures2 features2{};
                    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    features2.pNext = &probe_ext;
                    vkGetPhysicalDeviceFeatures2(physical_device, &features2);
                    has_nonuniform =
                        probe_ext.shaderSampledImageArrayNonUniformIndexing ==
                        VK_TRUE;
                    has_partial =
                        probe_ext.descriptorBindingPartiallyBound == VK_TRUE;
                    has_uab =
                        probe_ext.descriptorBindingSampledImageUpdateAfterBind ==
                        VK_TRUE;
                }

                // The table is one bindless_descriptor_limit-slot
                // sampled-image binding in the fragment stage: the
                // update-after-bind limits must cover it when UAB is
                // available, the core limits otherwise.
                const uint32_t table = options.bindless_descriptor_limit;
                const bool limits_ok =
                    table <= props.limits.maxPerStageDescriptorSampledImages &&
                    table <= props.limits.maxDescriptorSetSampledImages &&
                    (!has_uab ||
                     (table <=
                          di_props.maxPerStageDescriptorUpdateAfterBindSampledImages &&
                      table <=
                          di_props.maxDescriptorSetUpdateAfterBindSampledImages));
                caps.nonuniform = has_nonuniform;
                caps.partially_bound = has_partial;
                caps.update_after_bind = has_uab;
                caps.limits_cover_request = limits_ok;
                tier_miss = "a needed feature or limit is missing";
            }
        }
    }
    indexing = decide_indexing(
        caps, options.want_bindless && options.bindless_descriptor_limit > 0);
    if (indexing.bindless) {
        if (core_12) {
            enable_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            // descriptorIndexing is the aggregate switch: it must be on
            // for any of the specific features to work on a 1.2 device.
            enable_12.descriptorIndexing = VK_TRUE;
            enable_12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            enable_12.descriptorBindingPartiallyBound = VK_TRUE;
            enable_12.descriptorBindingSampledImageUpdateAfterBind =
                indexing.update_after_bind ? VK_TRUE : VK_FALSE;
            enable_chain = &enable_12;
        } else {
            device_extensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            enable_ext.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
            enable_ext.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            enable_ext.descriptorBindingPartiallyBound = VK_TRUE;
            enable_ext.descriptorBindingSampledImageUpdateAfterBind =
                indexing.update_after_bind ? VK_TRUE : VK_FALSE;
            enable_chain = &enable_ext;
        }
    }
    if (indexing.bindless) {
        core::info("gfx: texture tier: bindless global table ({})",
                   indexing.update_after_bind ? "update-after-bind writes"
                                              : "idle-flush writes");
    } else {
        core::info("gfx: texture tier: chunked tables ({})", tier_miss);
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pNext = enable_chain;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount =
        static_cast<uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();
    create_info.pEnabledFeatures = &enabled;
    if (vkCreateDevice(physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
        return core::fail(kErrDevice, "gfx", "vkCreateDevice failed");
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return {};
}

core::Status Gpu::Impl::create_allocator() {
    // VMA fetches every other function at runtime through these two
    // (VMA_DYNAMIC_VULKAN_FUNCTIONS, see vma_impl.cpp) - since 3.2 they
    // must be handed over explicitly; VMA no longer picks up the globals
    // by itself. Create the allocator only after volkLoadInstance and
    // volkLoadDevice have filled the pointers.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;  // volk globals
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo create_info{};
    create_info.physicalDevice = physical_device;
    create_info.device = device;
    create_info.instance = instance;
    create_info.vulkanApiVersion = VK_API_VERSION_1_0;
    create_info.pVulkanFunctions = &functions;
    if (vmaCreateAllocator(&create_info, &allocator) != VK_SUCCESS) {
        return core::fail(kErrAllocator, "gfx",
                          "failed to create the memory allocator");
    }
    return {};
}

void Gpu::Impl::destroy_surface() {
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
}

void Gpu::Impl::destroy_device_stage() {
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    // The null-state contract: every device-stage getter reports null/0
    // again, so a post-shutdown consumer never reads stale facts.
    physical_device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queue_family = 0;
    depth_format = 0;
    indexing = DescriptorIndexing{};
    device_stage_up = false;
}

void Gpu::Impl::destroy_instance_stage() {
    if (instance != VK_NULL_HANDLE) {
        // The messenger rides the instance - destroyed first, while its
        // vkDestroyDebugUtilsMessengerEXT entry point is still loaded.
        if (validation_messenger != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance, validation_messenger, nullptr);
            validation_messenger = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        // Must be last: this zeroes every function pointer volk loaded,
        // so no vk* call may happen after it.
        volkFinalize();
    }
    instance_up = false;
}

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
    shutdown();
}

core::Status Gpu::init(const SurfaceSource& source, const DeviceOptions& options) {
    if (impl_->instance_up) {
        return core::fail(kErrOrder, "gfx",
                          "init() on an already-initialized Gpu - one instance "
                          "per process");
    }
    if (source.instance_extension_count > 0 && source.instance_extensions == nullptr) {
        return core::fail(kErrOrder, "gfx",
                          "SurfaceSource lists extensions with a null pointer");
    }
    if (source.create_surface == nullptr) {
        return core::fail(kErrOrder, "gfx", "SurfaceSource has no create_surface callback");
    }
    // The extension-name POINTERS are copied once, right here - the source
    // array itself may be a caller temporary; only the strings it points
    // at must outlive the Gpu (SurfaceSource's contract). The stored
    // source keeps just the surface callback + user for later attaches.
    impl_->extensions.assign(source.instance_extensions,
                             source.instance_extensions + source.instance_extension_count);
    impl_->source = source;
    impl_->source.instance_extensions = nullptr;  // consumed above
    impl_->source.instance_extension_count = 0;
    impl_->options = options;

    const core::Status status = impl_->create_instance();
    if (!status) {
        return status;
    }
    impl_->instance_up = true;
    return {};
}

core::Status Gpu::attach_surface() {
    if (!impl_->instance_up) {
        return core::fail(kErrOrder, "gfx", "attach_surface() before init()");
    }
    if (impl_->surface != VK_NULL_HANDLE) {
        return core::fail(kErrOrder, "gfx",
                          "attach_surface() while a surface is already attached");
    }
    void* surface = nullptr;
    if (!impl_->source.create_surface(impl_->instance, &surface, impl_->source.user) ||
        surface == nullptr) {
        return core::fail(kErrSurface, "gfx", "failed to create the window surface");
    }
    impl_->surface = static_cast<VkSurfaceKHR>(surface);
    if (!impl_->device_stage_up) {
        // First attach also brings up the device stage: present support is
        // queried against the surface, so selection cannot run before one
        // exists; the stage then survives every detach.
        const core::Status selected = impl_->select_device();
        if (!selected) {
            impl_->destroy_surface();
            return selected;
        }
        const core::Status created = impl_->create_device();
        if (!created) {
            impl_->destroy_surface();
            return created;
        }
        const core::Status allocated = impl_->create_allocator();
        if (!allocated) {
            impl_->destroy_surface();
            return allocated;
        }
        impl_->device_stage_up = true;
    }
    return {};
}

void Gpu::detach_surface() {
    impl_->destroy_surface();
}

void Gpu::shutdown() {
    // Idempotent, in reverse attach order: the surface rides the window,
    // the allocator/device ride the device stage, the messenger/instance/
    // volk globals ride the context stage. Every destroy is guarded, so
    // this doubles as the failure-path cleanup for a partially completed
    // init() or attach_surface().
    impl_->destroy_surface();
    impl_->destroy_device_stage();
    impl_->destroy_instance_stage();
}

void* Gpu::instance() const {
    return impl_->instance;  // VkInstance
}

void* Gpu::surface() const {
    return impl_->surface;  // VkSurfaceKHR (window stage only)
}

void* Gpu::physical_device() const {
    return impl_->physical_device;  // VkPhysicalDevice (device stage)
}

void* Gpu::device() const {
    return impl_->device;  // VkDevice (device stage)
}

void* Gpu::queue() const {
    return impl_->queue;  // VkQueue (device stage)
}

void* Gpu::allocator() const {
    return impl_->allocator;  // VmaAllocator (device stage)
}

uint32_t Gpu::queue_family() const {
    return impl_->queue_family;
}

uint32_t Gpu::depth_format() const {
    return impl_->depth_format;  // VkFormat, numeric
}

DescriptorIndexing Gpu::descriptor_indexing() const {
    return impl_->indexing;
}

}  // namespace ws::gfx
