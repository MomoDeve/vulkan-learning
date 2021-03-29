#define GLFW_INCLUDE_VULKAN
#define STB_IMAGE_IMPLEMENTATION
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>
#include <stb_image.h>

#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>
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

struct BufferData
{
    vk::Buffer Buffer;
    vk::DeviceMemory DeviceMemory;
    void* HostMemory = nullptr;
};

struct ImageData
{
    vk::Image Image;
    vk::DeviceMemory Memory;
    vk::ImageView View;
};

struct DescriptorSetData
{
    vk::DescriptorSetLayout Layout;
    vk::DescriptorPool Pool;
    vk::DescriptorSet Set;
};

struct VirtualFrame
{
    vk::CommandBuffer CommandBuffer;
    vk::Fence CommandQueueFence;
    vk::Framebuffer Framebuffer;
};

constexpr size_t VirtualFrameCount = 3;

struct VulkanStaticData
{
    vk::Instance Instance;
    vk::PhysicalDevice PhysicalDevice;
    vk::Device Device;
    vk::CommandPool CommandPool;
    vk::SurfaceKHR Surface;
    vk::SurfaceCapabilitiesKHR SurfaceCapabilities;
    vk::Extent2D SurfaceExtent;
    vk::SurfaceFormatKHR SurfaceFormat;
    vk::PresentModeKHR SurfacePresentMode;
    uint32_t PresentImageCount;
    vk::Semaphore RenderingFinishedSemaphore;
    vk::Semaphore ImageAvailableSemaphore;
    vk::RenderPass MainRenderPass; 
    ImageData Texture;
    vk::Sampler TextureSampler;
    std::array<VirtualFrame, VirtualFrameCount> VirtualFrames; 
    std::vector<vk::ImageView> SwapchainImageViews;
    BufferData VertexBuffer;
    BufferData StagingBuffer;
    DescriptorSetData DescriptorSet;
    vk::Pipeline GraphicPipeline;
    vk::PipelineLayout GraphicPipelineLayout;
    vk::Queue DeviceQueue;
    vk::SwapchainKHR Swapchain;
    uint32_t FamilyQueueIndex;
} VulkanInstance;

std::vector<char> ReadFileAsBinary(const std::string& filename)
{
    std::vector<char> result;
    std::ifstream file(filename, std::ios_base::binary);
    if (!file.good()) std::cerr << "cannot open file: " << filename << std::endl;
    result = std::vector<char>(std::istreambuf_iterator(file), std::istreambuf_iterator<char>());
    return result;
}

auto CreateShaderModule(const std::string& filename)
{
    auto bytecode = ReadFileAsBinary(filename);
    vk::ShaderModuleCreateInfo createInfo;
    createInfo
        .setPCode(reinterpret_cast<const uint32_t*>(bytecode.data()))
        .setCodeSize(bytecode.size());

    return VulkanInstance.Device.createShaderModuleUnique(createInfo);
}

void UpdateSurfaceExtent(VulkanStaticData& vulkan, int newSurfaceWidth, int newSurfaceHeight)
{
    VulkanInstance.SurfaceCapabilities = vulkan.PhysicalDevice.getSurfaceCapabilitiesKHR(VulkanInstance.Surface);
    VulkanInstance.SurfaceExtent = vk::Extent2D(
        std::clamp((uint32_t)newSurfaceWidth, VulkanInstance.SurfaceCapabilities.minImageExtent.width, VulkanInstance.SurfaceCapabilities.maxImageExtent.width),
        std::clamp((uint32_t)newSurfaceHeight, VulkanInstance.SurfaceCapabilities.minImageExtent.height, VulkanInstance.SurfaceCapabilities.maxImageExtent.height)
    );
}

struct VertexData
{
    glm::vec4 Position;
    glm::vec2 TexCoord;
};

void RecreateFramebuffer(VulkanStaticData& vulkan, VirtualFrame& frame, size_t presentImageIndex)
{
    if ((bool)frame.Framebuffer)
    {
        vulkan.Device.destroyFramebuffer(frame.Framebuffer);
    }

    vk::FramebufferCreateInfo framebufferCreateInfo;
    framebufferCreateInfo
        .setRenderPass(VulkanInstance.MainRenderPass)
        .setAttachments(vulkan.SwapchainImageViews[presentImageIndex])
        .setHeight(vulkan.SurfaceExtent.height)
        .setWidth(vulkan.SurfaceExtent.width)
        .setLayers(1);

    frame.Framebuffer = vulkan.Device.createFramebuffer(framebufferCreateInfo);
}

void WriteCommandBuffer(VulkanStaticData& vulkan, VirtualFrame& frame)
{
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    commandBufferBeginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    frame.CommandBuffer.begin(commandBufferBeginInfo);

    frame.CommandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eBottomOfPipe,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        { }, // dependency flags
        { }, // memory barriers
        { }, // buffer memory barriers
        { }  // image memory barriers
    );

    vk::ClearColorValue clearColor = std::array{ 1.0f, 0.8f, 0.4f, 0.0f };
    vk::ClearValue clearValue;
    clearValue.setColor(clearColor);

    vk::Rect2D renderArea(vk::Offset2D{ 0, 0 }, vulkan.SurfaceExtent);

    vk::RenderPassBeginInfo renderPassBeginInfo;
    renderPassBeginInfo
        .setRenderPass(vulkan.MainRenderPass)
        .setFramebuffer(frame.Framebuffer)
        .setClearValues(clearValue)
        .setRenderArea(renderArea);

    frame.CommandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

    frame.CommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, vulkan.GraphicPipeline);

    vk::Viewport viewport = { 0.0f, 0.0f, (float)vulkan.SurfaceExtent.width, (float)vulkan.SurfaceExtent.height, 0.0f, 1.0f };
    frame.CommandBuffer.setViewport(0, viewport);

    vk::Rect2D scissor = { vk::Offset2D{ 0, 0 }, vulkan.SurfaceExtent };
    frame.CommandBuffer.setScissor(0, scissor);

    frame.CommandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, vulkan.GraphicPipelineLayout, 0, vulkan.DescriptorSet.Set, { });
    frame.CommandBuffer.bindVertexBuffers(0, vulkan.VertexBuffer.Buffer, { 0 });

    frame.CommandBuffer.draw(6, 1, 0, 0);

    frame.CommandBuffer.endRenderPass();

    frame.CommandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eBottomOfPipe,
        { }, // dependency flags
        { }, // memory barriers
        { }, // buffer memory barriers
        { }  // image memory barriers
    );

    frame.CommandBuffer.end();
}

void ProcessFrame(VulkanStaticData& vulkan, VirtualFrame& frame)
{
    vk::Result waitFenceResult = vulkan.Device.waitForFences(frame.CommandQueueFence, false, UINT64_MAX);
    if (waitFenceResult != vk::Result::eSuccess)
    {
        std::cerr << "waiting for fence failed due to timeout" << std::endl;
        return;
    }
    vulkan.Device.resetFences(frame.CommandQueueFence);

    auto acquireNextImage = vulkan.Device.acquireNextImageKHR(vulkan.Swapchain, UINT64_MAX, vulkan.ImageAvailableSemaphore);
    if (acquireNextImage.result == vk::Result::eNotReady)
    {
        std::cerr << "acquiring next image failed, image was not ready" << std::endl;
        return;
    }

    RecreateFramebuffer(vulkan, frame, acquireNextImage.value);
    WriteCommandBuffer(vulkan, frame);

    std::array waitDstStageMask = { (vk::PipelineStageFlags)vk::PipelineStageFlagBits::eTransfer };

    vk::SubmitInfo submitInfo;
    submitInfo
        .setWaitSemaphores(vulkan.ImageAvailableSemaphore)
        .setWaitDstStageMask(waitDstStageMask)
        .setSignalSemaphores(vulkan.RenderingFinishedSemaphore)
        .setCommandBuffers(frame.CommandBuffer);

    VulkanInstance.DeviceQueue.submit(std::array{ submitInfo }, frame.CommandQueueFence);

    vk::PresentInfoKHR presentInfo;
    presentInfo
        .setWaitSemaphores(vulkan.RenderingFinishedSemaphore)
        .setSwapchains(vulkan.Swapchain)
        .setImageIndices(acquireNextImage.value);

    auto presetSucceeded = VulkanInstance.DeviceQueue.presentKHR(presentInfo);
    assert(presetSucceeded == vk::Result::eSuccess);
}

BufferData CreateBuffer(VulkanStaticData& vulkan, size_t allocationSize, vk::BufferUsageFlags usageFlags, vk::MemoryPropertyFlagBits memoryProps)
{
    BufferData result;

    std::array bufferQueueFamiliyIndicies = { (uint32_t)0 };

    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        .setSize(allocationSize)
        .setUsage(usageFlags)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setQueueFamilyIndices(bufferQueueFamiliyIndicies);

    result.Buffer = vulkan.Device.createBuffer(bufferCreateInfo);

    vk::MemoryRequirements bufferMemoryRequirements = vulkan.Device.getBufferMemoryRequirements(result.Buffer);
    vk::PhysicalDeviceMemoryProperties memoryProperties = vulkan.PhysicalDevice.getMemoryProperties();

    size_t memoryTypeIndex = 0;
    for (const auto& property : memoryProperties.memoryTypes)
    {
        if (property.propertyFlags & memoryProps)
        {
            vk::MemoryAllocateInfo memoryAllocateInfo;
            memoryAllocateInfo
                .setAllocationSize(bufferMemoryRequirements.size)
                .setMemoryTypeIndex(memoryTypeIndex);

            result.DeviceMemory = vulkan.Device.allocateMemory(memoryAllocateInfo);
            vulkan.Device.bindBufferMemory(result.Buffer, result.DeviceMemory, 0);
            std::cout << "allocated buffer memory (" << bufferMemoryRequirements.size << " bytes)\n";
            break;
        }
        memoryTypeIndex++;
    }
    if (!(bool)result.DeviceMemory) std::cerr << "cannot find requested memory type for buffer" << std::endl;

    return result;
}

void InitializeStagingBuffer(VulkanStaticData& vulkan)
{
    constexpr size_t StagingBufferSize = 1024 * 1024 * 256;

    vulkan.StagingBuffer = CreateBuffer(
        VulkanInstance,
        StagingBufferSize,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible
    );
    std::cout << "staging buffer created\n";

    vulkan.StagingBuffer.HostMemory = vulkan.Device.mapMemory(vulkan.StagingBuffer.DeviceMemory, 0, StagingBufferSize);
}

void InitializeVertexBuffer(VulkanStaticData& vulkan)
{
    std::array vertexData = {
       VertexData {
           glm::vec4 { -0.9f, -0.9f, 0.0f, 1.0f },
           glm::vec2 { 0.0f, 0.0f },
       },
       VertexData {
           glm::vec4 { -0.9f, 0.9f, 0.0f, 1.0f },
           glm::vec2 { 0.0f, 1.0f },
       },
       VertexData {
           glm::vec4 { 0.9f, -0.9f, 0.0f, 1.0f },
           glm::vec2 { 1.0f, 0.0f },
       },
       VertexData {
           glm::vec4 { 0.9f, 0.9f, 0.0f, 1.0f },
           glm::vec2 { 1.0f, 1.0f },
       },
       VertexData {
           glm::vec4 { 0.9f, -0.9f, 0.0f, 1.0f },
           glm::vec2 { 1.0f, 0.0f },
       },
       VertexData {
           glm::vec4 { -0.9f, 0.9f, 0.0f, 1.0f },
           glm::vec2 { 0.0f, 1.0f },
       },
    };
    constexpr size_t VertexBufferSize = vertexData.size() * sizeof(VertexData);

    vulkan.VertexBuffer = CreateBuffer(
        VulkanInstance,
        VertexBufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal
    );

    std::memcpy(vulkan.StagingBuffer.HostMemory, (const void*)vertexData.data(), VertexBufferSize);

    vk::MappedMemoryRange flushRange;
    flushRange
        .setMemory(vulkan.StagingBuffer.DeviceMemory)
        .setSize(VertexBufferSize)
        .setOffset(0);

    vulkan.Device.flushMappedMemoryRanges(flushRange);

    vk::CommandBuffer& commandBuffer = vulkan.VirtualFrames.front().CommandBuffer;

    commandBuffer.begin(vk::CommandBufferBeginInfo{ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    vk::BufferCopy bufferCopyInfo;
    bufferCopyInfo
        .setSrcOffset(0)
        .setDstOffset(0)
        .setSize(VertexBufferSize);
    commandBuffer.copyBuffer(vulkan.StagingBuffer.Buffer, vulkan.VertexBuffer.Buffer, bufferCopyInfo);
        
    vk::BufferMemoryBarrier bufferCopyMemoryBarrier;
    bufferCopyMemoryBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eMemoryWrite)
        .setDstAccessMask(vk::AccessFlagBits::eVertexAttributeRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(vulkan.VertexBuffer.Buffer)
        .setSize(VertexBufferSize)
        .setOffset(0);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexInput, 
        { }, // dependency flags
        { }, // memory barriers
        bufferCopyMemoryBarrier, 
        { }  // image memory barriers
    );

    commandBuffer.end();

    vk::SubmitInfo bufferCopySubmitInfo;
    bufferCopySubmitInfo.setCommandBuffers(commandBuffer);

    vulkan.DeviceQueue.submit(bufferCopySubmitInfo);
    vulkan.Device.waitIdle();
}

void InitializeCommandBuffers(VulkanStaticData& vulkan)
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        .setQueueFamilyIndex(vulkan.FamilyQueueIndex)
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient);

    vulkan.CommandPool = vulkan.Device.createCommandPool(commandPoolCreateInfo);
    std::cout << "command pool created\n";

    for (auto& virtualFrame : vulkan.VirtualFrames)
    {
        // create command buffer
        vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
        commandBufferAllocateInfo
            .setCommandPool(vulkan.CommandPool)
            .setLevel(vk::CommandBufferLevel::ePrimary)
            .setCommandBufferCount(1);

        virtualFrame.CommandBuffer = vulkan.Device.allocateCommandBuffers(commandBufferAllocateInfo).front();
        // create command buffer fence
        virtualFrame.CommandQueueFence = vulkan.Device.createFence(vk::FenceCreateInfo{ vk::FenceCreateFlagBits::eSignaled });
    }
}

void InitializeDescriptorSet(VulkanStaticData& vulkan)
{
    vk::DescriptorSetLayoutBinding layoutBinding;
    layoutBinding
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eFragment);

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayout;
    descriptorSetLayout.setBindings(layoutBinding);

    vulkan.DescriptorSet.Layout = vulkan.Device.createDescriptorSetLayout(descriptorSetLayout);

    vk::DescriptorPoolSize poolSize;
    poolSize
        .setType(vk::DescriptorType::eCombinedImageSampler)
        .setDescriptorCount(1);

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(1)
        .setPoolSizes(poolSize);

    vulkan.DescriptorSet.Pool = vulkan.Device.createDescriptorPool(descriptorPoolCreateInfo);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(vulkan.DescriptorSet.Pool)
        .setSetLayouts(vulkan.DescriptorSet.Layout);

    vulkan.DescriptorSet.Set = vulkan.Device.allocateDescriptorSets(descriptorSetAllocateInfo).front();

    vk::DescriptorImageInfo descriptorImageInfo;
    descriptorImageInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(vulkan.Texture.View)
        .setSampler(vulkan.TextureSampler);

    vk::WriteDescriptorSet writeDescriptorSet;
    writeDescriptorSet
        .setDescriptorCount(1)
        .setDstSet(vulkan.DescriptorSet.Set)
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(descriptorImageInfo);

    vulkan.Device.updateDescriptorSets(writeDescriptorSet, { });
}

void InitializeRenderPass(VulkanStaticData& vulkan)
{
    vk::AttachmentDescription attachmentDescription;
    attachmentDescription
        .setFormat(VulkanInstance.SurfaceFormat.format)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::ePresentSrcKHR)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);

    vk::AttachmentReference colorAttachmentReference;
    colorAttachmentReference
        .setAttachment(0)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal);

    vk::SubpassDescription subpassDescription;
    subpassDescription
        .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        .setColorAttachments(colorAttachmentReference);

    std::array dependencies = {
        vk::SubpassDependency {
            VK_SUBPASS_EXTERNAL,
            0,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::AccessFlagBits::eMemoryRead,
            vk::AccessFlagBits::eColorAttachmentWrite,
            vk::DependencyFlagBits::eByRegion
        },
        vk::SubpassDependency {
            0,
            VK_SUBPASS_EXTERNAL,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::AccessFlagBits::eColorAttachmentWrite,
            vk::AccessFlagBits::eMemoryRead,
            vk::DependencyFlagBits::eByRegion
        },
    };

    vk::RenderPassCreateInfo renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(attachmentDescription)
        .setSubpasses(subpassDescription)
        .setDependencies(dependencies);

    vulkan.MainRenderPass = vulkan.Device.createRenderPass(renderPassCreateInfo);
    std::cout << "render pass created\n";
}

void InitializeGraphicPipeline(VulkanStaticData& vulkan)
{
    auto mainVertexShader = CreateShaderModule("main_vertex.spv");
    auto mainFragmentShader = CreateShaderModule("main_fragment.spv");
    std::cout << "main shader created\n";

    std::array shaderStageCreateInfos = {
    vk::PipelineShaderStageCreateInfo {
        vk::PipelineShaderStageCreateFlags{ },
        vk::ShaderStageFlagBits::eVertex,
        *mainVertexShader,
        "main"
    },
    vk::PipelineShaderStageCreateInfo {
        vk::PipelineShaderStageCreateFlags{ },
        vk::ShaderStageFlagBits::eFragment,
        *mainFragmentShader,
        "main"
    }
    };

    std::array vertexBindingDescriptions = {
    vk::VertexInputBindingDescription {
        0,
        sizeof(VertexData),
        vk::VertexInputRate::eVertex
    }
    };

    std::array vertexAttributeDescriptions = {
        vk::VertexInputAttributeDescription {
            0,
            vertexBindingDescriptions[0].binding,
            vk::Format::eR32G32B32A32Sfloat,
            offsetof(VertexData, Position)
        },
        vk::VertexInputAttributeDescription {
            1,
            vertexBindingDescriptions[0].binding,
            vk::Format::eR32G32Sfloat,
            offsetof(VertexData, TexCoord)
        }
    };

    vk::PipelineVertexInputStateCreateInfo vertexInputStateCreateInfo;
    vertexInputStateCreateInfo
        .setVertexBindingDescriptions(vertexBindingDescriptions)
        .setVertexAttributeDescriptions(vertexAttributeDescriptions);

    vk::PipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo;
    inputAssemblyStateCreateInfo
        .setPrimitiveRestartEnable(false)
        .setTopology(vk::PrimitiveTopology::eTriangleList);

    vk::PipelineViewportStateCreateInfo viewportStateCreateInfo;
    viewportStateCreateInfo
        .setViewportCount(1) // defined dynamic
        .setScissorCount(1); // defined dynamic

    vk::PipelineRasterizationStateCreateInfo rasterizationStateCreateInfo;
    rasterizationStateCreateInfo
        .setPolygonMode(vk::PolygonMode::eFill)
        .setCullMode(vk::CullModeFlagBits::eBack)
        .setFrontFace(vk::FrontFace::eCounterClockwise)
        .setLineWidth(1.0f);

    vk::PipelineMultisampleStateCreateInfo multisampleStateCreateInfo;
    multisampleStateCreateInfo
        .setRasterizationSamples(vk::SampleCountFlagBits::e1)
        .setMinSampleShading(1.0f);

    vk::PipelineColorBlendAttachmentState colorBlendAttachmentState;
    colorBlendAttachmentState
        .setBlendEnable(false)
        .setSrcColorBlendFactor(vk::BlendFactor::eOne)
        .setDstColorBlendFactor(vk::BlendFactor::eZero)
        .setColorBlendOp(vk::BlendOp::eAdd)
        .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
        .setDstAlphaBlendFactor(vk::BlendFactor::eZero)
        .setAlphaBlendOp(vk::BlendOp::eAdd)
        .setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);

    vk::PipelineColorBlendStateCreateInfo colorBlendStateCreateInfo;
    colorBlendStateCreateInfo
        .setLogicOpEnable(false)
        .setLogicOp(vk::LogicOp::eCopy)
        .setAttachments(colorBlendAttachmentState)
        .setBlendConstants({ 0.0f, 0.0f, 0.0f, 0.0f });

    vk::PipelineLayoutCreateInfo layoutCreateInfo;
    layoutCreateInfo.setSetLayouts(vulkan.DescriptorSet.Layout);

    vulkan.GraphicPipelineLayout = vulkan.Device.createPipelineLayout(layoutCreateInfo);

    std::array dynamicStates = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };

    vk::PipelineDynamicStateCreateInfo dynamicStateCreateInfo;
    dynamicStateCreateInfo.setDynamicStates(dynamicStates);

    vk::GraphicsPipelineCreateInfo pipelineCreateInfo;
    pipelineCreateInfo
        .setStages(shaderStageCreateInfos)
        .setPVertexInputState(&vertexInputStateCreateInfo)
        .setPInputAssemblyState(&inputAssemblyStateCreateInfo)
        .setPTessellationState(nullptr)
        .setPViewportState(&viewportStateCreateInfo)
        .setPRasterizationState(&rasterizationStateCreateInfo)
        .setPMultisampleState(&multisampleStateCreateInfo)
        .setPDepthStencilState(nullptr)
        .setPColorBlendState(&colorBlendStateCreateInfo)
        .setPDynamicState(&dynamicStateCreateInfo)
        .setLayout(vulkan.GraphicPipelineLayout)
        .setRenderPass(vulkan.MainRenderPass)
        .setSubpass(0)
        .setBasePipelineHandle(vk::Pipeline{ })
        .setBasePipelineIndex(0);

    auto pipeline = vulkan.Device.createGraphicsPipeline(vk::PipelineCache{ }, pipelineCreateInfo);
    if (pipeline.result != vk::Result::eSuccess)
        std::cerr << "cannot create vk::Pipeline: " + vk::to_string(pipeline.result) << std::endl;

    vulkan.GraphicPipeline = pipeline.value;
    std::cout << "graphic pipeline created\n";
}

ImageData CreateImage(VulkanStaticData& vulkan, size_t width, size_t height)
{
    ImageData result;

    vk::ImageCreateInfo imageCreateInfo;
    imageCreateInfo
        .setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setExtent(vk::Extent3D{ (uint32_t)width, (uint32_t)height, 1 })
        .setSamples(vk::SampleCountFlagBits::e1)
        .setMipLevels(1)
        .setArrayLayers(1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    result.Image = vulkan.Device.createImage(imageCreateInfo);

    vk::MemoryRequirements imageMemoryRequirements = vulkan.Device.getImageMemoryRequirements(result.Image);
    vk::PhysicalDeviceMemoryProperties memoryProperties = vulkan.PhysicalDevice.getMemoryProperties();

    size_t memoryTypeIndex = 0;
    for (const auto& property : memoryProperties.memoryTypes)
    {
        if (property.propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
        {
            vk::MemoryAllocateInfo memoryAllocateInfo;
            memoryAllocateInfo
                .setAllocationSize(imageMemoryRequirements.size)
                .setMemoryTypeIndex(memoryTypeIndex);

            result.Memory = vulkan.Device.allocateMemory(memoryAllocateInfo);
            vulkan.Device.bindImageMemory(result.Image, result.Memory, 0);
            std::cout << "allocated image memory (" << imageMemoryRequirements.size << " bytes)\n";
            break;
        }
        memoryTypeIndex++;
    }
    if (!(bool)result.Memory) std::cerr << "cannot find requested memory type for image" << std::endl;

    return result;
}

void InitializeTexture(VulkanStaticData& vulkan)
{
    int width, height, channels;
    const unsigned char* textureData = stbi_load("vulkan-logo.png", &width, &height, &channels, 4);
    if (textureData == nullptr)
    {
        std::cerr << "cannot load texture file" << std::endl;
    }
    size_t textureByteSize = size_t(width * height * 4);

    vulkan.Texture = CreateImage(vulkan, (size_t)width, (size_t)height);

    vk::ImageSubresourceRange subresourceRange {
            vk::ImageAspectFlagBits::eColor,
            0, // base mip level
            1, // level count
            0, // base layer
            1  // layer count
    };

    vk::ImageViewCreateInfo imageViewCreateInfo;
    imageViewCreateInfo
        .setImage(vulkan.Texture.Image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setComponents(vk::ComponentMapping {
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity 
        })
        .setSubresourceRange(subresourceRange);

    vulkan.Texture.View = vulkan.Device.createImageView(imageViewCreateInfo);

    std::memcpy(vulkan.StagingBuffer.HostMemory, (const void*)textureData, textureByteSize);
    vk::MappedMemoryRange flushRange;
    flushRange
        .setMemory(vulkan.StagingBuffer.DeviceMemory)
        .setSize(textureByteSize)
        .setOffset(0);

    vulkan.Device.flushMappedMemoryRanges(flushRange);

    vk::CommandBuffer& commandBuffer = vulkan.VirtualFrames.front().CommandBuffer;

    commandBuffer.begin(vk::CommandBufferBeginInfo{ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    vk::ImageMemoryBarrier imageTransferMemoryBarrier;
    imageTransferMemoryBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eNoneKHR)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(vulkan.Texture.Image)
        .setSubresourceRange(subresourceRange);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer,
        { }, // dependency flags
        { }, // memory barriers
        { }, // buffer barriers
        imageTransferMemoryBarrier
    );

    vk::BufferImageCopy imageCopyInfo;
    imageCopyInfo
        .setBufferOffset(0)
        .setBufferRowLength(0)
        .setBufferImageHeight(0)
        .setImageSubresource(vk::ImageSubresourceLayers {
            vk::ImageAspectFlagBits::eColor,
            0, // base mip level
            0, // base layer
            1  // layer count
        })
        .setImageOffset(vk::Offset3D{ 0, 0, 0 })
        .setImageExtent(vk::Extent3D{ (uint32_t)width, (uint32_t)height, 1 });

    commandBuffer.copyBufferToImage(vulkan.StagingBuffer.Buffer, vulkan.Texture.Image, vk::ImageLayout::eTransferDstOptimal, imageCopyInfo);

    vk::ImageMemoryBarrier imageCopyMemoryBarrier;
    imageCopyMemoryBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(vulkan.Texture.Image)
        .setSubresourceRange(subresourceRange);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eFragmentShader,
        { }, // dependency flags
        { }, // memory barriers
        { }, // buffer barriers
        imageCopyMemoryBarrier
    );

    commandBuffer.end();

    vk::SubmitInfo bufferCopySubmitInfo;
    bufferCopySubmitInfo.setCommandBuffers(commandBuffer);

    vulkan.DeviceQueue.submit(bufferCopySubmitInfo);

    vulkan.Device.waitIdle();
    stbi_image_free((void*)textureData);
}

void InitializeTextureSampler(VulkanStaticData& vulkan)
{
    vk::SamplerCreateInfo samplerCreateInfo;
    samplerCreateInfo
        .setMagFilter(vk::Filter::eLinear)
        .setMinFilter(vk::Filter::eLinear)
        .setMipmapMode(vk::SamplerMipmapMode::eLinear)
        .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setMipLodBias(0.0f)
        .setAnisotropyEnable(false)
        .setMaxAnisotropy(1.0f)
        .setCompareEnable(false)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMinLod(0.0f)
        .setMaxLod(0.0f)
        .setBorderColor(vk::BorderColor::eFloatTransparentBlack)
        .setUnnormalizedCoordinates(false);

    vulkan.TextureSampler = vulkan.Device.createSampler(samplerCreateInfo);
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
    std::cout << "vk::SwapChainKHR created\n";
    if (bool(swapchainCreateInfo.oldSwapchain))
    {
        vulkan.Device.destroySwapchainKHR(swapchainCreateInfo.oldSwapchain);
    }

    auto swapchainImages = vulkan.Device.getSwapchainImagesKHR(vulkan.Swapchain);

    // create framebuffers
    vulkan.SwapchainImageViews.resize(vulkan.PresentImageCount);
    for (uint32_t i = 0; i < vulkan.PresentImageCount; i++)
    {
        vk::ImageSubresourceRange imageSubresourceRange;
        imageSubresourceRange
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setBaseMipLevel(0)
            .setLevelCount(1)
            .setBaseArrayLayer(0)
            .setLayerCount(1);

        vk::ImageViewCreateInfo imageViewCreateInfo;
        imageViewCreateInfo
            .setImage(swapchainImages[i])
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(vulkan.SurfaceFormat.format)
            .setSubresourceRange(imageSubresourceRange)
            .setComponents(vk::ComponentMapping {
                vk::ComponentSwizzle::eIdentity,
                vk::ComponentSwizzle::eIdentity,
                vk::ComponentSwizzle::eIdentity,
                vk::ComponentSwizzle::eIdentity
            });

        if ((bool)vulkan.SwapchainImageViews[i])
            vulkan.Device.destroyImageView(vulkan.SwapchainImageViews[i]);

        vulkan.SwapchainImageViews[i] = vulkan.Device.createImageView(imageViewCreateInfo);
    }
    std::cout << "swapchain image views created\n";
}

int main()
{
    std::filesystem::current_path(APPLICATION_WORKING_DIRECTORY);

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
    const int windowWidth = 1200;
    const int windowHeight = 400;
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
    std::array queuePriorities = { 1.0f };
    deviceQueueCreateInfo.setQueueFamilyIndex(VulkanInstance.FamilyQueueIndex);
    deviceQueueCreateInfo.setQueuePriorities(queuePriorities);

    vk::DeviceCreateInfo deviceCreateInfo;
    std::array extenstionNames = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    deviceCreateInfo.setQueueCreateInfos(deviceQueueCreateInfo);
    deviceCreateInfo.setPEnabledExtensionNames(extenstionNames);
    
    VulkanInstance.Device = VulkanInstance.PhysicalDevice.createDevice(deviceCreateInfo);
    std::cout << "vk::Device created\n";
    VulkanInstance.DeviceQueue = VulkanInstance.Device.getQueue(VulkanInstance.FamilyQueueIndex, 0);

    VulkanInstance.ImageAvailableSemaphore = VulkanInstance.Device.createSemaphore(vk::SemaphoreCreateInfo{ });
    VulkanInstance.RenderingFinishedSemaphore = VulkanInstance.Device.createSemaphore(vk::SemaphoreCreateInfo{ });

    auto SwapchainCreator = [](GLFWwindow* window, int width, int height) { std::cout << "recreating swapchain...\n"; RecreateSwapchain(VulkanInstance, width, height); };
    RecreateSwapchain(VulkanInstance, windowWidth, windowHeight);
    glfwSetWindowSizeCallback(window, SwapchainCreator);

    auto swapchainImages = VulkanInstance.Device.getSwapchainImagesKHR(VulkanInstance.Swapchain);

    InitializeCommandBuffers(VulkanInstance);
    InitializeStagingBuffer(VulkanInstance); 
    InitializeVertexBuffer(VulkanInstance);
    InitializeTexture(VulkanInstance);
    InitializeTextureSampler(VulkanInstance);
    InitializeTextureSampler(VulkanInstance);
    InitializeDescriptorSet(VulkanInstance);
    InitializeRenderPass(VulkanInstance);
    InitializeGraphicPipeline(VulkanInstance);

    size_t virtualFrameIndex = 0;
    int framesSinceMeasure = 0;
    double measureStartTime = glfwGetTime();
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ProcessFrame(VulkanInstance, VulkanInstance.VirtualFrames[virtualFrameIndex]);

        if ((++framesSinceMeasure) == 360)
        {
            double currentTime = glfwGetTime();
            auto frameCount = int(framesSinceMeasure / (currentTime - measureStartTime));
            glfwSetWindowTitle(window, ("vulkan-learning " + std::to_string(frameCount) + " FPS").c_str());
            measureStartTime = glfwGetTime();
            framesSinceMeasure = 0;
        }

        virtualFrameIndex = (virtualFrameIndex + 1) % VirtualFrameCount;
    }

    VulkanInstance.Device.waitIdle();

    VulkanInstance.Device.destroyBuffer(VulkanInstance.VertexBuffer.Buffer);
    VulkanInstance.Device.destroyBuffer(VulkanInstance.StagingBuffer.Buffer);

    VulkanInstance.Device.destroyImage(VulkanInstance.Texture.Image);
    VulkanInstance.Device.destroyImageView(VulkanInstance.Texture.View);
    VulkanInstance.Device.destroySampler(VulkanInstance.TextureSampler);

    VulkanInstance.Device.destroyDescriptorPool(VulkanInstance.DescriptorSet.Pool);
    VulkanInstance.Device.destroyDescriptorSetLayout(VulkanInstance.DescriptorSet.Layout);

    VulkanInstance.Device.destroyRenderPass(VulkanInstance.MainRenderPass);
    for (const auto& virtualFrame : VulkanInstance.VirtualFrames)
    {
        VulkanInstance.Device.destroyFramebuffer(virtualFrame.Framebuffer);
        VulkanInstance.Device.destroyFence(virtualFrame.CommandQueueFence);
    }

    VulkanInstance.Device.destroySemaphore(VulkanInstance.RenderingFinishedSemaphore);
    VulkanInstance.Device.destroySemaphore(VulkanInstance.ImageAvailableSemaphore);

    for (const auto& imageView : VulkanInstance.SwapchainImageViews)
    {
        VulkanInstance.Device.destroyImageView(imageView);
    }

    VulkanInstance.Device.destroyPipeline(VulkanInstance.GraphicPipeline);
    VulkanInstance.Device.destroyPipelineLayout(VulkanInstance.GraphicPipelineLayout);

    VulkanInstance.Device.destroyCommandPool(VulkanInstance.CommandPool);

    VulkanInstance.Device.destroySwapchainKHR(VulkanInstance.Swapchain);

    VulkanInstance.Device.destroy();
    VulkanInstance.Instance.destroySurfaceKHR(VulkanInstance.Surface);
    VulkanInstance.Instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}