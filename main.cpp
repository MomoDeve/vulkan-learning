#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.hpp>

#include <iostream>
#include <vector>
#include <optional>

bool CheckDeviceProperties(const vk::Instance& instance, const vk::PhysicalDevice& device, const vk::PhysicalDeviceProperties& properties, uint32_t& queueFamilyIndex)
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

    vk::Instance instance = vk::createInstance(createInfo);
    auto extensions = vk::enumerateInstanceExtensionProperties();

    std::cout << "\navailable extensions:\n";
    for (const auto& extension : extensions) 
    {
        std::cout << '\t' << extension.extensionName << '\n';
    }

    // acquire physical devices

    auto physicalDevices = instance.enumeratePhysicalDevices();
    std::cout << "\nphysical devices:\n";
    vk::PhysicalDevice currentPhysicalDevice;
    uint32_t currentQueueFamilyIndex = 0;
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
            
        if (CheckDeviceProperties(instance, device, properties, currentQueueFamilyIndex))
        {
            currentPhysicalDevice = device;
        }

        std::cout << std::endl;
    }
    if(!bool(currentPhysicalDevice))
    {
        std::cerr << "no suitable physical device was found\n";
        return 0;
    }
    std::cout << "selected device: " << currentPhysicalDevice.getProperties().deviceName << '\n';

    vk::DeviceQueueCreateInfo deviceQueueCreateInfo;
    deviceQueueCreateInfo.setQueueFamilyIndex(currentQueueFamilyIndex);
    deviceQueueCreateInfo.setQueuePriorities(std::array{ 1.0f });

    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo.setQueueCreateInfos(std::array{ deviceQueueCreateInfo });
    
    vk::Device logicalDevice = currentPhysicalDevice.createDevice(deviceCreateInfo);
    vk::Queue deviceQueue = logicalDevice.getQueue(currentQueueFamilyIndex, 0);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "vulkan-learning", nullptr, nullptr);
    vk::SurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, window, nullptr, (VkSurfaceKHR*)&surface) != VkResult::VK_SUCCESS)
    {
        std::cerr << "cannot create surface\n";
        return 0;
    }

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
    }

    instance.destroySurfaceKHR(surface);
    logicalDevice.destroy();
    instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}