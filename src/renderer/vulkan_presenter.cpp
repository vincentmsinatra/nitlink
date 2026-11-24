#include "vulkan_presenter.h"

#include <dxgi1_2.h>
#include <wrl/client.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace NitLink {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kNoImage = UINT32_MAX;
constexpr auto kD3DTexture = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags read, VkAccessFlags write)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = read;
    barrier.dstAccessMask = write;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}
}

struct VulkanPresenter::Impl {
    ~Impl();
    bool CreateBridge(ID3D11Device* d3d, UINT width, UINT height, DXGI_FORMAT dxgiFormat,
                      VkFormat vkFormat);
    void ReleaseSwapChain();
    void ReleaseBridge();
    VkResult Acquire(uint64_t timeoutNs);

    HMODULE loader = nullptr;
    vk::detail::DispatchLoaderDynamic vk;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence submitted = VK_NULL_HANDLE;
    VkFence imageAcquired = VK_NULL_HANDLE;
    VkSemaphore imageReady = VK_NULL_HANDLE;
    VkSwapchainKHR swapChain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    // Reacquiring an image proves that its previous present semaphore was consumed.
    std::vector<VkSemaphore> presentReady;
    uint32_t acquired = kNoImage;
    bool acquireFencePending = false;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<IDXGIKeyedMutex> mutex;
    VkImage bridge = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool hdrMetadata = false;
    bool needsResize = false;
    bool failed = false;
};

VulkanPresenter::Impl::~Impl()
{
    if (device) {
        vk.vkDeviceWaitIdle(device);
        ReleaseSwapChain();
        ReleaseBridge();
        if (submitted) vk.vkDestroyFence(device, submitted, nullptr);
        if (pool) vk.vkDestroyCommandPool(device, pool, nullptr);
        vk.vkDestroyDevice(device, nullptr);
    }
    if (surface) vk.vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vk.vkDestroyInstance(instance, nullptr);
    if (loader) FreeLibrary(loader);
}

VulkanPresenter::VulkanPresenter() : m_state(std::make_unique<Impl>()) {}
VulkanPresenter::~VulkanPresenter() = default;

bool VulkanPresenter::Initialize(HWND window, const LUID& adapter)
{
    auto& s = *m_state;
    s.loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.loader) return false;
    const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(s.loader, "vkGetInstanceProcAddr"));
    if (!getProc) return false;
    s.vk.init(getProc);

    uint32_t count = 0;
    if (s.vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (s.vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    if (!HasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !HasExtension(extensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) return false;
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                       VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "NitLink";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = HasExtension(extensions, instanceExtensions[2]) ? 3 : 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    if (s.vk.vkCreateInstance(&instanceInfo, nullptr, &s.instance) != VK_SUCCESS) return false;
    s.vk.init(vk::Instance{s.instance});

    VkWin32SurfaceCreateInfoKHR windowInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    windowInfo.hinstance = GetModuleHandleW(nullptr);
    windowInfo.hwnd = window;
    if (s.vk.vkCreateWin32SurfaceKHR(s.instance, &windowInfo, nullptr, &s.surface) != VK_SUCCESS) return false;

    if (s.vk.vkEnumeratePhysicalDevices(s.instance, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkPhysicalDevice> adapters(count);
    if (s.vk.vkEnumeratePhysicalDevices(s.instance, &count, adapters.data()) != VK_SUCCESS) return false;
    for (auto candidate : adapters) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        s.vk.vkGetPhysicalDeviceProperties2(candidate, &properties);
        if (properties.properties.apiVersion >= VK_API_VERSION_1_1 && id.deviceLUIDValid &&
            std::memcmp(id.deviceLUID, &adapter, VK_LUID_SIZE) == 0) {
            s.physical = candidate;
            break;
        }
    }
    if (!s.physical) return false;

    s.vk.vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    s.vk.vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &count, families.data());
    for (; s.family < count; ++s.family) {
        VkBool32 supported = VK_FALSE;
        if ((families[s.family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            s.vk.vkGetPhysicalDeviceSurfaceSupportKHR(s.physical, s.family, s.surface, &supported) == VK_SUCCESS &&
            supported) break;
    }
    if (s.family == count) return false;
    if (s.vk.vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    extensions.resize(count);
    if (s.vk.vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME, VK_EXT_HDR_METADATA_EXTENSION_NAME};
    for (unsigned i = 0; i < 3; ++i) if (!HasExtension(extensions, deviceExtensions[i])) return false;
    s.hdrMetadata = HasExtension(extensions, deviceExtensions[3]);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = s.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = s.hdrMetadata ? 4 : 3;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (s.vk.vkCreateDevice(s.physical, &deviceInfo, nullptr, &s.device) != VK_SUCCESS) return false;
    s.vk.init(vk::Device{s.device});
    s.vk.vkGetDeviceQueue(s.device, s.family, 0, &s.queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = s.family;
    if (s.vk.vkCreateCommandPool(s.device, &poolInfo, nullptr, &s.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = s.pool;
    commandInfo.commandBufferCount = 1;
    if (s.vk.vkAllocateCommandBuffers(s.device, &commandInfo, &s.commands) != VK_SUCCESS) return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return s.vk.vkCreateFence(s.device, &fenceInfo, nullptr, &s.submitted) == VK_SUCCESS;
}

bool VulkanPresenter::Impl::CreateBridge(ID3D11Device* d3d, UINT width, UINT height,
                                          DXGI_FORMAT dxgiFormat, VkFormat format)
{
    VkPhysicalDeviceExternalImageFormatInfo externalFormat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    externalFormat.handleType = kD3DTexture;
    VkPhysicalDeviceImageFormatInfo2 formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalFormat};
    formatInfo.format = format;
    formatInfo.type = VK_IMAGE_TYPE_2D;
    formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    formatInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalProperties};
    if (vk.vkGetPhysicalDeviceImageFormatProperties2(physical, &formatInfo, &properties) != VK_SUCCESS ||
        !(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<IDXGIResource1> resource;
    if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &sharedTexture)) ||
        FAILED(sharedTexture.As(&resource)) || FAILED(sharedTexture.As(&mutex))) return false;

    VkExternalMemoryImageCreateInfo externalImage{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalImage.handleTypes = kD3DTexture;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &externalImage};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {width, height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vk.vkCreateImage(device, &image, nullptr, &bridge) != VK_SUCCESS) return false;

    HANDLE handle = nullptr;
    if (FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                            nullptr, &handle))) return false;
    VkMemoryWin32HandlePropertiesKHR handleProperties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(device, bridge, &requirements);
    VkResult result = vk.vkGetMemoryWin32HandlePropertiesKHR(device, kD3DTexture, handle, &handleProperties);
    const uint32_t memoryTypes = requirements.memoryTypeBits & handleProperties.memoryTypeBits;
    if (result == VK_SUCCESS && memoryTypes) {
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = bridge;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
        import.handleType = kD3DTexture;
        import.handle = handle;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
        allocation.allocationSize = requirements.size;
        while ((memoryTypes & (1u << allocation.memoryTypeIndex)) == 0) ++allocation.memoryTypeIndex;
        result = vk.vkAllocateMemory(device, &allocation, nullptr, &memory);
    }
    CloseHandle(handle);
    return result == VK_SUCCESS && memory &&
           vk.vkBindImageMemory(device, bridge, memory, 0) == VK_SUCCESS;
}

void VulkanPresenter::Impl::ReleaseBridge()
{
    if (bridge) vk.vkDestroyImage(device, bridge, nullptr);
    if (memory) vk.vkFreeMemory(device, memory, nullptr);
    bridge = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    mutex.Reset();
    sharedTexture.Reset();
}

void VulkanPresenter::Impl::ReleaseSwapChain()
{
    // A prepared image may never reach queue submission if the window changes.
    // In that case its WSI acquire still owns the signal on imageReady.
    if (acquireFencePending) {
        vk.vkWaitForFences(device, 1, &imageAcquired, VK_TRUE, 1'000'000'000);
    }
    for (auto semaphore : presentReady) {
        if (semaphore) vk.vkDestroySemaphore(device, semaphore, nullptr);
    }
    presentReady.clear();
    images.clear();
    if (imageReady) vk.vkDestroySemaphore(device, imageReady, nullptr);
    if (imageAcquired) vk.vkDestroyFence(device, imageAcquired, nullptr);
    if (swapChain) vk.vkDestroySwapchainKHR(device, swapChain, nullptr);
    imageReady = VK_NULL_HANDLE;
    imageAcquired = VK_NULL_HANDLE;
    swapChain = VK_NULL_HANDLE;
    acquired = kNoImage;
    acquireFencePending = false;
}

bool VulkanPresenter::Resize(ID3D11Device* d3d, UINT width, UINT height, DXGI_FORMAT format, bool vsync)
{
    auto& s = *m_state;
    const bool hdr = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    const VkFormat imageFormat = hdr ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_B8G8R8A8_UNORM;
    const auto colorspace = hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkSurfaceCapabilitiesKHR caps{};
    if (s.vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, s.surface, &caps) != VK_SUCCESS ||
        !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) return false;
    const VkExtent2D extent{width, height};
    if (width < caps.minImageExtent.width || width > caps.maxImageExtent.width ||
        height < caps.minImageExtent.height || height > caps.maxImageExtent.height ||
        (caps.currentExtent.width != UINT32_MAX &&
         (width != caps.currentExtent.width || height != caps.currentExtent.height))) return false;

    uint32_t count = 0;
    if (s.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (s.vk.vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, s.surface, &count, formats.data()) != VK_SUCCESS ||
        std::none_of(formats.begin(), formats.end(), [&](auto entry) {
            return entry.format == imageFormat && entry.colorSpace == colorspace;
        })) return false;
    if (s.vk.vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, s.surface, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkPresentModeKHR> modes(count);
    if (s.vk.vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, s.surface, &count, modes.data()) != VK_SUCCESS) return false;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        for (auto candidate : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
            if (std::find(modes.begin(), modes.end(), candidate) != modes.end()) {
                mode = candidate;
                break;
            }
        }
    }

    // Window and output changes are infrequent. The live present path never
    // waits for the previous frame's commands to finish on the GPU.
    if (s.vk.vkDeviceWaitIdle(s.device) != VK_SUCCESS) return false;
    s.ReleaseBridge();
    if (!s.CreateBridge(d3d, width, height, format, imageFormat)) {
        s.failed = true;
        return false;
    }
    VkSwapchainCreateInfoKHR swap{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swap.surface = s.surface;
    swap.minImageCount = (std::max)(caps.minImageCount, 2u);
    if (caps.maxImageCount) swap.minImageCount = (std::min)(caps.maxImageCount, swap.minImageCount);
    swap.imageFormat = imageFormat;
    swap.imageColorSpace = colorspace;
    swap.imageExtent = extent;
    swap.imageArrayLayers = 1;
    swap.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swap.preTransform = caps.currentTransform;
    swap.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swap.presentMode = mode;
    swap.clipped = VK_TRUE;
    swap.oldSwapchain = s.swapChain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult result = s.vk.vkCreateSwapchainKHR(s.device, &swap, nullptr, &created);
    s.ReleaseSwapChain();
    s.swapChain = created;
    s.failed = true;
    if (result != VK_SUCCESS ||
        s.vk.vkGetSwapchainImagesKHR(s.device, created, &count, nullptr) != VK_SUCCESS) return false;
    s.images.resize(count);
    if (s.vk.vkGetSwapchainImagesKHR(s.device, created, &count, s.images.data()) != VK_SUCCESS) return false;
    s.images.resize(count);
    s.presentReady.resize(count);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo acquireFence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (s.vk.vkCreateSemaphore(s.device, &semaphore, nullptr, &s.imageReady) != VK_SUCCESS ||
        s.vk.vkCreateFence(s.device, &acquireFence, nullptr, &s.imageAcquired) != VK_SUCCESS) return false;
    for (auto& ready : s.presentReady) {
        if (s.vk.vkCreateSemaphore(s.device, &semaphore, nullptr, &ready) != VK_SUCCESS) return false;
    }
    if (hdr && s.hdrMetadata) {
        VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        metadata.displayPrimaryRed = {0.708f, 0.292f};
        metadata.displayPrimaryGreen = {0.170f, 0.797f};
        metadata.displayPrimaryBlue = {0.131f, 0.046f};
        metadata.whitePoint = {0.3127f, 0.3290f};
        metadata.maxLuminance = 1000.0f;
        metadata.minLuminance = 0.005f;
        metadata.maxContentLightLevel = 1000.0f;
        metadata.maxFrameAverageLightLevel = 400.0f;
        s.vk.vkSetHdrMetadataEXT(s.device, 1, &s.swapChain, &metadata);
    }
    s.extent = extent;
    s.needsResize = false;
    s.failed = false;
    return true;
}

VkResult VulkanPresenter::Impl::Acquire(uint64_t timeoutNs)
{
    if (failed || !swapChain) return VK_ERROR_DEVICE_LOST;
    if (needsResize) return VK_ERROR_OUT_OF_DATE_KHR;
    const VkResult fence = vk.vkGetFenceStatus(device, submitted);
    if (fence != VK_SUCCESS) return fence;
    if (acquired != kNoImage) return VK_SUCCESS;
    if (acquireFencePending) {
        const VkResult ready = vk.vkGetFenceStatus(device, imageAcquired);
        if (ready != VK_SUCCESS) return ready;
        const VkResult reset = vk.vkResetFences(device, 1, &imageAcquired);
        if (reset != VK_SUCCESS) return reset;
        acquireFencePending = false;
    }
    const VkResult result = vk.vkAcquireNextImageKHR(device, swapChain, timeoutNs, imageReady, imageAcquired, &acquired);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        acquireFencePending = true;
        return VK_SUCCESS;
    }
    acquired = kNoImage;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) needsResize = true;
    return result;
}

void VulkanPresenter::WaitForImage()
{
    // No software cap is active. Wait for image availability with a bound so
    // minimized windows and display transitions still service window messages.
    const auto result = m_state->Acquire(10'000'000);
    if (result != VK_SUCCESS && result != VK_TIMEOUT) Sleep(1);
}

void VulkanPresenter::RequestResize() { m_state->needsResize = true; }

bool VulkanPresenter::NeedsResize() const { return m_state->needsResize; }

HRESULT VulkanPresenter::Present(ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    auto& s = *m_state;
    const VkResult acquired = s.Acquire(0);
    if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT || acquired == VK_ERROR_OUT_OF_DATE_KHR) return S_FALSE;
    if (acquired != VK_SUCCESS) return DXGI_ERROR_DEVICE_REMOVED;

    const HRESULT lock = s.mutex->AcquireSync(0, 0);
    if (lock == WAIT_TIMEOUT) return S_FALSE;
    if (lock != S_OK) return DXGI_ERROR_DEVICE_REMOVED;
    context->CopyResource(s.sharedTexture.Get(), source);
    context->Flush();
    if (FAILED(s.mutex->ReleaseSync(1))) return DXGI_ERROR_DEVICE_REMOVED;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (s.vk.vkResetCommandBuffer(s.commands, 0) != VK_SUCCESS ||
        s.vk.vkBeginCommandBuffer(s.commands, &begin) != VK_SUCCESS) return DXGI_ERROR_DEVICE_REMOVED;
    const auto target = s.images[s.acquired];
    VkImageMemoryBarrier barriers[] = {
        ImageBarrier(s.bridge, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_READ_BIT),
        ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT)
    };
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barriers[0].dstQueueFamilyIndex = s.family;
    s.vk.vkCmdPipelineBarrier(s.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {s.extent.width, s.extent.height, 1};
    s.vk.vkCmdCopyImage(s.commands, s.bridge, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barriers[0] = ImageBarrier(s.bridge, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_ACCESS_TRANSFER_READ_BIT, 0);
    barriers[0].srcQueueFamilyIndex = s.family;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barriers[1] = ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    s.vk.vkCmdPipelineBarrier(s.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    if (s.vk.vkEndCommandBuffer(s.commands) != VK_SUCCESS ||
        s.vk.vkResetFences(s.device, 1, &s.submitted) != VK_SUCCESS) return DXGI_ERROR_DEVICE_REMOVED;

    const uint64_t acquireKey = 1, releaseKey = 0;
    const uint32_t timeoutMs = 1000;
    VkWin32KeyedMutexAcquireReleaseInfoKHR mutexInfo{VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    mutexInfo.acquireCount = mutexInfo.releaseCount = 1;
    mutexInfo.pAcquireSyncs = mutexInfo.pReleaseSyncs = &s.memory;
    mutexInfo.pAcquireKeys = &acquireKey;
    mutexInfo.pAcquireTimeouts = &timeoutMs;
    mutexInfo.pReleaseKeys = &releaseKey;
    const auto ready = s.presentReady[s.acquired];
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &mutexInfo};
    submit.waitSemaphoreCount = submit.commandBufferCount = submit.signalSemaphoreCount = 1;
    submit.pWaitSemaphores = &s.imageReady;
    submit.pWaitDstStageMask = &waitStage;
    submit.pCommandBuffers = &s.commands;
    submit.pSignalSemaphores = &ready;
    if (s.vk.vkQueueSubmit(s.queue, 1, &submit, s.submitted) != VK_SUCCESS) return DXGI_ERROR_DEVICE_REMOVED;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = present.swapchainCount = 1;
    present.pWaitSemaphores = &ready;
    present.pSwapchains = &s.swapChain;
    present.pImageIndices = &s.acquired;
    const auto result = s.vk.vkQueuePresentKHR(s.queue, &present);
    s.acquired = kNoImage;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        s.needsResize = true;
        return S_FALSE;
    }
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
}

} // namespace NitLink
