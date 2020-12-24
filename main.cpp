#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.hpp>

#include <iostream>
#include <vector>
#include <algorithm>

bool CheckDeviceProperties(const vk::Instance& instance, const vk::PhysicalDevice& device, const vk::PhysicalDeviceProperties& properties, const vk::SurfaceKHR surface, uint32_t& queueFamilyIndex)
{
    if (!(VK_VERSION_MAJOR(properties.apiVersion) == 1 && VK_VERSION_MINOR(properties.apiVersion) == 2))
    {
        std::cout << "failed to select " << properties.deviceName << ": device does not support Vulkan 1.2\n";
        return false;
    }

    auto queueFamilyProperties = device.getQueueFamilyProperties();
    uint32_t index = 0;
    for (const auto& property : queueFamilyProperties)
    {
        if ((property.queueCount > 0) &&
            (device.getSurfaceSupportKHR(index, surface)) &&
            (glfwGetPhysicalDevicePresentationSupport(instance, device, index)) &&
            (property.queueFlags & vk::QueueFlagBits::eGraphics) &&
            (property.queueFlags & vk::QueueFlagBits::eCompute))
        {
            queueFamilyIndex = index;
            return true;
        }

        index++;
    }

    std::cout << "failed to select " << properties.deviceName << ": device does not support graphic & compute queue families\n";

    return false;
}

struct VulkanStaticData
{
    vk::Instance Instance;
    vk::PhysicalDevice PhysicalDevice;
    vk::Device Device;
    vk::CommandPool CommandPool;
    std::vector<vk::CommandBuffer> CommandBuffers;
    vk::SurfaceKHR Surface;
    vk::SurfaceCapabilitiesKHR SurfaceCapabilities;
    vk::Extent2D SurfaceExtent;
    vk::SurfaceFormatKHR SurfaceFormat;
    vk::PresentModeKHR SurfacePresentMode;
    uint32_t PresentImageCount;
    vk::Semaphore RenderingFinishedSemaphore;
    vk::Semaphore ImageAvailableSemaphore;
    vk::Queue DeviceQueue;
    vk::SwapchainKHR Swapchain;
    uint32_t FamilyQueueIndex;
} VulkanInstance;

void UpdateSurfaceExtent(VulkanStaticData& vulkan, int newSurfaceWidth, int newSurfaceHeight)
{
    VulkanInstance.SurfaceCapabilities = vulkan.PhysicalDevice.getSurfaceCapabilitiesKHR(VulkanInstance.Surface);
    VulkanInstance.SurfaceExtent = vk::Extent2D(
        std::clamp((uint32_t)newSurfaceWidth, VulkanInstance.SurfaceCapabilities.minImageExtent.width, VulkanInstance.SurfaceCapabilities.maxImageExtent.width),
        std::clamp((uint32_t)newSurfaceHeight, VulkanInstance.SurfaceCapabilities.minImageExtent.height, VulkanInstance.SurfaceCapabilities.maxImageExtent.height)
    );
}

void RecreateSwapchain(VulkanStaticData& vulkan, int newSurfaceWidth, int newSurfaceHeight)
{
    vulkan.Device.waitIdle();

    UpdateSurfaceExtent(vulkan, newSurfaceWidth, newSurfaceHeight);

    vk::SwapchainCreateInfoKHR swapchainCreateInfo;
    swapchainCreateInfo
        .setSurface(vulkan.Surface)
        .setMinImageCount(vulkan.PresentImageCount)
        .setImageFormat(vulkan.SurfaceFormat.format)
        .setImageColorSpace(vulkan.SurfaceFormat.colorSpace)
        .setImageExtent(vulkan.SurfaceExtent)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setImageSharingMode(vk::SharingMode::eExclusive)
        .setPreTransform(vulkan.SurfaceCapabilities.currentTransform)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setPresentMode(vulkan.SurfacePresentMode)
        .setClipped(true)
        .setOldSwapchain(vulkan.Swapchain);

    vulkan.Swapchain = vulkan.Device.createSwapchainKHR(swapchainCreateInfo);
    if (bool(swapchainCreateInfo.oldSwapchain))
        vulkan.Device.destroySwapchainKHR(swapchainCreateInfo.oldSwapchain);

    if (bool(vulkan.CommandPool)) vulkan.Device.destroyCommandPool(vulkan.CommandPool);
    vulkan.CommandPool = vulkan.Device.createCommandPool(vk::CommandPoolCreateInfo{ { }, vulkan.FamilyQueueIndex });

    vulkan.CommandBuffers = vulkan.Device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        vulkan.CommandPool,
        vk::CommandBufferLevel::ePrimary,
        vulkan.PresentImageCount
    });

    vk::CommandBufferBeginInfo commandBeginInfo;
    commandBeginInfo.setFlags(vk::CommandBufferUsageFlagBits::eSimultaneousUse);

    vk::ClearColorValue clearColor{ std::array{1.0f, 0.8f, 0.4f, 0.0f} };
    vk::ImageSubresourceRange imageSubresourceRange;
    imageSubresourceRange
        .setAspectMask(vk::ImageAspectFlagBits::eColor)
        .setBaseMipLevel(0)
        .setBaseArrayLayer(0)
        .setLevelCount(1)
        .setLayerCount(1);

    auto swapchainImages = vulkan.Device.getSwapchainImagesKHR(vulkan.Swapchain);
    for (uint32_t i = 0; i < vulkan.PresentImageCount; i++)
    {
        vk::ImageMemoryBarrier barrierFromPresentToClear;
        barrierFromPresentToClear
            .setSrcAccessMask(vk::AccessFlagBits::eMemoryRead)
            .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setOldLayout(vk::ImageLayout::eUndefined)
            .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
            .setSrcQueueFamilyIndex(vulkan.FamilyQueueIndex)
            .setDstQueueFamilyIndex(vulkan.FamilyQueueIndex)
            .setImage(swapchainImages[i])
            .setSubresourceRange(imageSubresourceRange);

        vk::ImageMemoryBarrier barrierFromClearToPresent;
        barrierFromClearToPresent
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead)
            .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
            .setNewLayout(vk::ImageLayout::ePresentSrcKHR)
            .setSrcQueueFamilyIndex(vulkan.FamilyQueueIndex)
            .setDstQueueFamilyIndex(vulkan.FamilyQueueIndex)
            .setImage(swapchainImages[i])
            .setSubresourceRange(imageSubresourceRange);

        vulkan.CommandBuffers[i].begin(commandBeginInfo);
        vulkan.CommandBuffers[i].pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags{ },
            std::array<vk::MemoryBarrier, 0>{ },
            std::array<vk::BufferMemoryBarrier, 0>{ },
            std::array{ barrierFromPresentToClear }
        );

        vulkan.CommandBuffers[i].clearColorImage(
            swapchainImages[i],
            vk::ImageLayout::eTransferDstOptimal,
            clearColor,
            std::array{ imageSubresourceRange }
        );

        vulkan.CommandBuffers[i].pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::DependencyFlags{ },
            std::array<vk::MemoryBarrier, 0>{ },
            std::array<vk::BufferMemoryBarrier, 0>{ },
            std::array{ barrierFromClearToPresent }
        );

        vulkan.CommandBuffers[i].end();
    }
}

int main()
{
    if (!glfwInit())
    {
        std::cerr << "cannot initialize GLFW\n";
        return 0;
    }
    if (!glfwVulkanSupported())
    {
        std::cerr << "GLFW version does not support vulkan api\n";
        return 0;
    }

    vk::ApplicationInfo appInfo;
    appInfo.setPApplicationName("vulkan-learning");
    appInfo.setApplicationVersion(VK_MAKE_VERSION(1, 0, 0));
    appInfo.setPEngineName("No Engine");
    appInfo.setEngineVersion(VK_MAKE_VERSION(1, 0, 0));
    appInfo.setApiVersion(VK_API_VERSION_1_2);

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::cout << "\nglfw extensions:\n";
    for (auto it = glfwExtensions; it != glfwExtensions + glfwExtensionCount; it++)
    {
        std::cout << '\t' << *it << '\n';
    }

    vk::InstanceCreateInfo createInfo;
    createInfo.setPApplicationInfo(&appInfo);
    createInfo.setEnabledExtensionCount(glfwExtensionCount);
    createInfo.setPpEnabledExtensionNames(glfwExtensions);
    createInfo.setEnabledLayerCount(0);

    VulkanInstance.Instance = vk::createInstance(createInfo);
    auto extensions = vk::enumerateInstanceExtensionProperties();

    std::cout << "\navailable extensions:\n";
    for (const auto& extension : extensions) 
    {
        std::cout << '\t' << extension.extensionName << '\n';
    }

    // create window

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    const int windowWidth = 1600;
    const int windowHeight = 900;
    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "vulkan-learning", nullptr, nullptr);
    if (glfwCreateWindowSurface(VulkanInstance.Instance, window, nullptr, (VkSurfaceKHR*)&VulkanInstance.Surface) != VkResult::VK_SUCCESS)
    {
        std::cerr << "cannot create surface\n";
        return 0;
    }

    // acquire physical devices

    auto physicalDevices = VulkanInstance.Instance.enumeratePhysicalDevices();
    std::cout << "\nphysical devices:\n";
    for (const auto& device : physicalDevices)
    {
        auto properties = device.getProperties();
        std::cout << "\tname: " << properties.deviceName << '\n';
        std::cout << "\tdevice type: " << vk::to_string(properties.deviceType) << '\n';

        auto majorVersion = VK_VERSION_MAJOR(properties.apiVersion);
        auto minorVersion = VK_VERSION_MINOR(properties.apiVersion);
        auto patchVersion = VK_VERSION_PATCH(properties.apiVersion);

        std::cout << "\tapi version: " << majorVersion << '.' << minorVersion << '.' << patchVersion << '\n';

        auto extenstions = device.enumerateDeviceExtensionProperties();
        std::cout << "\textensions:\n";
        for (const auto& extension : extensions)
        {
            std::cout << "\t\t" << extension.extensionName << '\n';
        }
            
        if (CheckDeviceProperties(VulkanInstance.Instance, device, properties, VulkanInstance.Surface, VulkanInstance.FamilyQueueIndex))
        {
            VulkanInstance.PhysicalDevice = device;
        }

        std::cout << std::endl;
    }
    if(!bool(VulkanInstance.PhysicalDevice))
    {
        std::cerr << "no suitable physical device was found\n";
        return 0;
    }

    std::cout << "selected device: " << VulkanInstance.PhysicalDevice.getProperties().deviceName << '\n';

    VulkanInstance.SurfaceCapabilities = VulkanInstance.PhysicalDevice.getSurfaceCapabilitiesKHR(VulkanInstance.Surface);

    auto presentModes = VulkanInstance.PhysicalDevice.getSurfacePresentModesKHR(VulkanInstance.Surface);
    std::cout << "supported present modes:\n";
    for (const auto& presentMode : presentModes)
    {
        std::cout << '\t' << vk::to_string(presentMode) << '\n';
    }
    if (std::find(presentModes.begin(), presentModes.end(), vk::PresentModeKHR::eMailbox) != presentModes.end())
        VulkanInstance.SurfacePresentMode = vk::PresentModeKHR::eMailbox;
    else
        VulkanInstance.SurfacePresentMode = vk::PresentModeKHR::eImmediate;

    VulkanInstance.PresentImageCount = VulkanInstance.SurfaceCapabilities.minImageCount;
    if (VulkanInstance.SurfacePresentMode == vk::PresentModeKHR::eMailbox) VulkanInstance.PresentImageCount++;
    if (VulkanInstance.SurfaceCapabilities.maxImageCount > 0 &&
        VulkanInstance.SurfaceCapabilities.maxImageCount < VulkanInstance.PresentImageCount)
    {
        VulkanInstance.PresentImageCount = VulkanInstance.SurfaceCapabilities.maxImageCount;
    }

    UpdateSurfaceExtent(VulkanInstance, windowWidth, windowHeight);

    std::cout << "supported surface usage:\n";
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eColorAttachment)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eColorAttachment) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eDepthStencilAttachment)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eDepthStencilAttachment) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eFragmentDensityMapEXT)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eFragmentDensityMapEXT) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eInputAttachment)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eInputAttachment) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eSampled)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eSampled) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eShadingRateImageNV)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eShadingRateImageNV) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eStorage)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eStorage) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eTransferDst) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eTransferSrc) << '\n';
    if (VulkanInstance.SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransientAttachment)
        std::cout << '\t' << vk::to_string(vk::ImageUsageFlagBits::eTransientAttachment) << '\n';
    std::cout << std::endl;

    auto surfaceFormats = VulkanInstance.PhysicalDevice.getSurfaceFormatsKHR(VulkanInstance.Surface);
    std::cout << "supported surface formats:\n";
    for (const auto& format : surfaceFormats)
    {
        std::cout << '\t' << vk::to_string(format.format) << '\n';
        if (format.format == vk::Format::eR8G8B8A8Unorm || format.format == vk::Format::eB8G8R8A8Unorm)
            VulkanInstance.SurfaceFormat = format;
    }
    std::cout << std::endl;
    if (VulkanInstance.SurfaceFormat.format == vk::Format::eUndefined)
        VulkanInstance.SurfaceFormat = surfaceFormats.front();

    vk::DeviceQueueCreateInfo deviceQueueCreateInfo;
    deviceQueueCreateInfo.setQueueFamilyIndex(VulkanInstance.FamilyQueueIndex);
    deviceQueueCreateInfo.setQueuePriorities(std::array{ 1.0f });

    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo.setQueueCreateInfos(std::array{ deviceQueueCreateInfo });
    deviceCreateInfo.setPEnabledExtensionNames(std::array
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    });
    
    VulkanInstance.Device = VulkanInstance.PhysicalDevice.createDevice(deviceCreateInfo);
    VulkanInstance.DeviceQueue = VulkanInstance.Device.getQueue(VulkanInstance.FamilyQueueIndex, 0);

    VulkanInstance.ImageAvailableSemaphore = VulkanInstance.Device.createSemaphore(vk::SemaphoreCreateInfo{ });
    VulkanInstance.RenderingFinishedSemaphore = VulkanInstance.Device.createSemaphore(vk::SemaphoreCreateInfo{ });

    auto SwapchainCreator = [](GLFWwindow* window, int width, int height) { RecreateSwapchain(VulkanInstance, width, height); };
    SwapchainCreator(window, windowWidth, windowHeight);
    glfwSetWindowSizeCallback(window, SwapchainCreator);

    auto swapchainImages = VulkanInstance.Device.getSwapchainImagesKHR(VulkanInstance.Swapchain);


    int framesSinceMeasure = 0;
    double measureStartTime = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        auto acquireImageResult = VulkanInstance.Device.acquireNextImageKHR(VulkanInstance.Swapchain, UINT64_MAX, VulkanInstance.ImageAvailableSemaphore, vk::Fence{});
        if (acquireImageResult.result == vk::Result::eNotReady)
            std::cerr << "acquired image was not ready!" << std::endl;

        vk::Image renderTarget = swapchainImages[acquireImageResult.value];

        vk::SubmitInfo submitInfo;
        submitInfo
            .setWaitSemaphores(VulkanInstance.ImageAvailableSemaphore)
            .setWaitDstStageMask(std::array{ (vk::PipelineStageFlags)vk::PipelineStageFlagBits::eTransfer })
            .setSignalSemaphores(VulkanInstance.RenderingFinishedSemaphore)
            .setCommandBuffers(std::array{ VulkanInstance.CommandBuffers[acquireImageResult.value] });
        
        VulkanInstance.DeviceQueue.submit(std::array{ submitInfo }, vk::Fence{ });

        vk::PresentInfoKHR presentInfo;
        presentInfo
            .setWaitSemaphores(std::array{ VulkanInstance.RenderingFinishedSemaphore })
            .setSwapchains(std::array{ VulkanInstance.Swapchain })
            .setImageIndices(std::array{ acquireImageResult.value });

        VulkanInstance.DeviceQueue.presentKHR(presentInfo);

        if ((++framesSinceMeasure) == 360)
        {
            double currentTime = glfwGetTime();
            auto frameCount = int(framesSinceMeasure / (currentTime - measureStartTime));
            glfwSetWindowTitle(window, ("vulkan-learning " + std::to_string(frameCount) + " FPS").c_str());
            measureStartTime = glfwGetTime();
            framesSinceMeasure = 0;
        }
    }

    VulkanInstance.Device.waitIdle();

    VulkanInstance.Device.destroyCommandPool(VulkanInstance.CommandPool);

    VulkanInstance.Device.destroySwapchainKHR(VulkanInstance.Swapchain);

    VulkanInstance.Device.destroySemaphore(VulkanInstance.RenderingFinishedSemaphore);
    VulkanInstance.Device.destroySemaphore(VulkanInstance.ImageAvailableSemaphore);

    VulkanInstance.Device.destroy();
    VulkanInstance.Instance.destroySurfaceKHR(VulkanInstance.Surface);
    VulkanInstance.Instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}