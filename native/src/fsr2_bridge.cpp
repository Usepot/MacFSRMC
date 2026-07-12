#include <jni.h>

#include "macfsr_vulkan_shim.h"
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"
#include "macfsr_motion_reactive_permutations.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct DeviceFunctions {
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
};

struct ImageResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
};

struct alignas(16) MotionPushConstants {
    float currentToPreviousClip[16];
    float inverseRenderSize[2];
    float reactiveScale;
    uint32_t resetHistory;
};
static_assert(sizeof(MotionPushConstants) == 80);

struct BridgeContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    DeviceFunctions vk{};
    bool initialized = false;
    bool debug = false;

    FfxFsr2Context fsr{};
    bool fsrCreated = false;
    std::vector<uint8_t> scratch;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;

    ImageResource output;
    ImageResource motion;
    ImageResource reactive;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout motionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline motionPipeline = VK_NULL_HANDLE;
};

BridgeContext g_context;
std::mutex g_mutex;
std::string g_lastError;

void setError(const std::string& message) {
    g_lastError = message;
    if (g_context.debug) {
        std::cerr << "[MacFSRMC] " << message << std::endl;
    }
}

std::string resultMessage(const char* operation, VkResult result) {
    return std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result));
}

template <typename T>
bool loadDeviceFunction(T& destination, const char* name) {
    destination = reinterpret_cast<T>(macfsr_vkGetDeviceProcAddr(g_context.device, name));
    if (destination == nullptr) {
        setError(std::string("Missing Vulkan device function ") + name);
        return false;
    }
    return true;
}

bool loadDeviceFunctions() {
    DeviceFunctions& vk = g_context.vk;
    return loadDeviceFunction(vk.createImage, "vkCreateImage")
        && loadDeviceFunction(vk.destroyImage, "vkDestroyImage")
        && loadDeviceFunction(vk.getImageMemoryRequirements, "vkGetImageMemoryRequirements")
        && loadDeviceFunction(vk.allocateMemory, "vkAllocateMemory")
        && loadDeviceFunction(vk.freeMemory, "vkFreeMemory")
        && loadDeviceFunction(vk.bindImageMemory, "vkBindImageMemory")
        && loadDeviceFunction(vk.createImageView, "vkCreateImageView")
        && loadDeviceFunction(vk.destroyImageView, "vkDestroyImageView")
        && loadDeviceFunction(vk.createSampler, "vkCreateSampler")
        && loadDeviceFunction(vk.destroySampler, "vkDestroySampler")
        && loadDeviceFunction(vk.createDescriptorSetLayout, "vkCreateDescriptorSetLayout")
        && loadDeviceFunction(vk.destroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout")
        && loadDeviceFunction(vk.createDescriptorPool, "vkCreateDescriptorPool")
        && loadDeviceFunction(vk.destroyDescriptorPool, "vkDestroyDescriptorPool")
        && loadDeviceFunction(vk.allocateDescriptorSets, "vkAllocateDescriptorSets")
        && loadDeviceFunction(vk.updateDescriptorSets, "vkUpdateDescriptorSets")
        && loadDeviceFunction(vk.createShaderModule, "vkCreateShaderModule")
        && loadDeviceFunction(vk.destroyShaderModule, "vkDestroyShaderModule")
        && loadDeviceFunction(vk.createPipelineLayout, "vkCreatePipelineLayout")
        && loadDeviceFunction(vk.destroyPipelineLayout, "vkDestroyPipelineLayout")
        && loadDeviceFunction(vk.createComputePipelines, "vkCreateComputePipelines")
        && loadDeviceFunction(vk.destroyPipeline, "vkDestroyPipeline")
        && loadDeviceFunction(vk.cmdPipelineBarrier, "vkCmdPipelineBarrier")
        && loadDeviceFunction(vk.cmdBindPipeline, "vkCmdBindPipeline")
        && loadDeviceFunction(vk.cmdBindDescriptorSets, "vkCmdBindDescriptorSets")
        && loadDeviceFunction(vk.cmdPushConstants, "vkCmdPushConstants")
        && loadDeviceFunction(vk.cmdDispatch, "vkCmdDispatch")
        && loadDeviceFunction(vk.cmdBlitImage, "vkCmdBlitImage")
        && loadDeviceFunction(vk.deviceWaitIdle, "vkDeviceWaitIdle");
}

uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    macfsr_vkGetPhysicalDeviceMemoryProperties(g_context.physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    return UINT32_MAX;
}

void destroyImage(ImageResource& resource) {
    if (resource.view != VK_NULL_HANDLE) {
        g_context.vk.destroyImageView(g_context.device, resource.view, nullptr);
    }
    if (resource.image != VK_NULL_HANDLE) {
        g_context.vk.destroyImage(g_context.device, resource.image, nullptr);
    }
    if (resource.memory != VK_NULL_HANDLE) {
        g_context.vk.freeMemory(g_context.device, resource.memory, nullptr);
    }
    resource = {};
}

bool createImage(
    ImageResource& resource,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage
) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = g_context.vk.createImage(g_context.device, &imageInfo, nullptr, &resource.image);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateImage", result));
        return false;
    }

    VkMemoryRequirements requirements{};
    g_context.vk.getImageMemoryRequirements(g_context.device, resource.image, &requirements);
    uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
        setError("No device-local Vulkan memory type is compatible with an FSR 2 image");
        destroyImage(resource);
        return false;
    }

    VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = memoryType;
    result = g_context.vk.allocateMemory(g_context.device, &allocationInfo, nullptr, &resource.memory);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkAllocateMemory", result));
        destroyImage(resource);
        return false;
    }
    result = g_context.vk.bindImageMemory(g_context.device, resource.image, resource.memory, 0);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkBindImageMemory", result));
        destroyImage(resource);
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = resource.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = g_context.vk.createImageView(g_context.device, &viewInfo, nullptr, &resource.view);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateImageView", result));
        destroyImage(resource);
        return false;
    }

    resource.format = format;
    resource.width = width;
    resource.height = height;
    resource.initialized = false;
    return true;
}

void transitionImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageAspectFlags aspect,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage,
    VkAccessFlags sourceAccess,
    VkAccessFlags destinationAccess
) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    g_context.vk.cmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        VK_DEPENDENCY_BY_REGION_BIT,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );
}

void transitionOwnedImageToGeneral(VkCommandBuffer commandBuffer, ImageResource& resource) {
    if (resource.initialized) {
        return;
    }
    transitionImage(
        commandBuffer,
        resource.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    );
    resource.initialized = true;
}

void destroyMotionPipeline() {
    DeviceFunctions& vk = g_context.vk;
    if (g_context.motionPipeline != VK_NULL_HANDLE) {
        vk.destroyPipeline(g_context.device, g_context.motionPipeline, nullptr);
    }
    if (g_context.motionPipelineLayout != VK_NULL_HANDLE) {
        vk.destroyPipelineLayout(g_context.device, g_context.motionPipelineLayout, nullptr);
    }
    if (g_context.descriptorPool != VK_NULL_HANDLE) {
        vk.destroyDescriptorPool(g_context.device, g_context.descriptorPool, nullptr);
    }
    if (g_context.descriptorSetLayout != VK_NULL_HANDLE) {
        vk.destroyDescriptorSetLayout(g_context.device, g_context.descriptorSetLayout, nullptr);
    }
    if (g_context.sampler != VK_NULL_HANDLE) {
        vk.destroySampler(g_context.device, g_context.sampler, nullptr);
    }
    g_context.motionPipeline = VK_NULL_HANDLE;
    g_context.motionPipelineLayout = VK_NULL_HANDLE;
    g_context.descriptorPool = VK_NULL_HANDLE;
    g_context.descriptorSetLayout = VK_NULL_HANDLE;
    g_context.descriptorSet = VK_NULL_HANDLE;
    g_context.sampler = VK_NULL_HANDLE;
}

bool createMotionPipeline() {
    DeviceFunctions& vk = g_context.vk;
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;
    VkResult result = vk.createSampler(g_context.device, &samplerInfo, nullptr, &g_context.sampler);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateSampler", result));
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorCount = 1;
        bindings[index].descriptorType = index < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    result = vk.createDescriptorSetLayout(g_context.device, &layoutInfo, nullptr, &g_context.descriptorSetLayout);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateDescriptorSetLayout", result));
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}
    }};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    result = vk.createDescriptorPool(g_context.device, &poolInfo, nullptr, &g_context.descriptorPool);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateDescriptorPool", result));
        return false;
    }
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = g_context.descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &g_context.descriptorSetLayout;
    result = vk.allocateDescriptorSets(g_context.device, &setInfo, &g_context.descriptorSet);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkAllocateDescriptorSets", result));
        return false;
    }

    const macfsr_motion_reactive_PermutationInfo& shader = g_macfsr_motion_reactive_PermutationInfo[0];
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = shader.blobSize;
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(shader.blobData);
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    result = vk.createShaderModule(g_context.device, &shaderInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateShaderModule", result));
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(MotionPushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &g_context.descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    result = vk.createPipelineLayout(g_context.device, &pipelineLayoutInfo, nullptr, &g_context.motionPipelineLayout);
    if (result != VK_SUCCESS) {
        vk.destroyShaderModule(g_context.device, shaderModule, nullptr);
        setError(resultMessage("vkCreatePipelineLayout", result));
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = g_context.motionPipelineLayout;
    result = vk.createComputePipelines(g_context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_context.motionPipeline);
    vk.destroyShaderModule(g_context.device, shaderModule, nullptr);
    if (result != VK_SUCCESS) {
        setError(resultMessage("vkCreateComputePipelines", result));
        return false;
    }
    return true;
}

void fsrMessage(FfxFsr2MsgType type, const wchar_t* message) {
    if (!g_context.debug || message == nullptr) {
        return;
    }
    std::wcerr << (type == FFX_FSR2_MESSAGE_TYPE_ERROR ? L"[MacFSRMC/FSR2 ERROR] " : L"[MacFSRMC/FSR2] ") << message << std::endl;
}

void destroyFsrResources() {
    if (g_context.fsrCreated) {
        ffxFsr2ContextDestroy(&g_context.fsr);
        g_context.fsrCreated = false;
    }
    destroyMotionPipeline();
    destroyImage(g_context.output);
    destroyImage(g_context.motion);
    destroyImage(g_context.reactive);
    g_context.scratch.clear();
    g_context.renderWidth = 0;
    g_context.renderHeight = 0;
    g_context.displayWidth = 0;
    g_context.displayHeight = 0;
}

bool createFsrResources(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight) {
    if (g_context.fsrCreated
        && g_context.renderWidth == renderWidth
        && g_context.renderHeight == renderHeight
        && g_context.displayWidth == displayWidth
        && g_context.displayHeight == displayHeight) {
        return true;
    }

    if (g_context.fsrCreated) {
        VkResult idleResult = g_context.vk.deviceWaitIdle(g_context.device);
        if (idleResult != VK_SUCCESS) {
            setError(resultMessage("vkDeviceWaitIdle during FSR resize", idleResult));
            return false;
        }
    }
    destroyFsrResources();

    size_t scratchSize = ffxFsr2GetScratchMemorySizeVK(g_context.physicalDevice);
    if (scratchSize == 0) {
        setError("FSR 2 reported a zero-byte Vulkan backend scratch buffer");
        return false;
    }
    g_context.scratch.resize(scratchSize);
    FfxFsr2ContextDescription description{};
    FfxErrorCode error = ffxFsr2GetInterfaceVK(
        &description.callbacks,
        g_context.scratch.data(),
        g_context.scratch.size(),
        g_context.physicalDevice,
        macfsr_vkGetDeviceProcAddr
    );
    if (error != FFX_OK) {
        setError("ffxFsr2GetInterfaceVK failed with code " + std::to_string(error));
        return false;
    }
    description.device = ffxGetDeviceVK(g_context.device);
    description.maxRenderSize = {renderWidth, renderHeight};
    description.displaySize = {displayWidth, displayHeight};
    description.flags = FFX_FSR2_ENABLE_DEPTH_INVERTED
        | FFX_FSR2_ENABLE_DEPTH_INFINITE
        | FFX_FSR2_ENABLE_AUTO_EXPOSURE
        | FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (g_context.debug) {
        description.flags |= FFX_FSR2_ENABLE_DEBUG_CHECKING;
        description.fpMessage = fsrMessage;
    }
    error = ffxFsr2ContextCreate(&g_context.fsr, &description);
    if (error != FFX_OK) {
        setError("ffxFsr2ContextCreate failed with code " + std::to_string(error));
        destroyFsrResources();
        return false;
    }
    g_context.fsrCreated = true;

    bool resourcesCreated = createImage(
        g_context.output,
        displayWidth,
        displayHeight,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
    ) && createImage(
        g_context.motion,
        renderWidth,
        renderHeight,
        VK_FORMAT_R16G16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    ) && createImage(
        g_context.reactive,
        renderWidth,
        renderHeight,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    ) && createMotionPipeline();
    if (!resourcesCreated) {
        destroyFsrResources();
        return false;
    }

    g_context.renderWidth = renderWidth;
    g_context.renderHeight = renderHeight;
    g_context.displayWidth = displayWidth;
    g_context.displayHeight = displayHeight;
    return true;
}

void updateMotionDescriptors(VkImageView colorView, VkImageView depthView) {
    std::array<VkDescriptorImageInfo, 4> images{};
    images[0] = {g_context.sampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[1] = {g_context.sampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[2] = {VK_NULL_HANDLE, g_context.motion.view, VK_IMAGE_LAYOUT_GENERAL};
    images[3] = {VK_NULL_HANDLE, g_context.reactive.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = g_context.descriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = index < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &images[index];
    }
    g_context.vk.updateDescriptorSets(g_context.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void recordMotionPass(
    VkCommandBuffer commandBuffer,
    VkImage color,
    VkImageView colorView,
    VkImage depth,
    VkImageView depthView,
    const MotionPushConstants& pushConstants
) {
    transitionOwnedImageToGeneral(commandBuffer, g_context.output);
    transitionOwnedImageToGeneral(commandBuffer, g_context.motion);
    transitionOwnedImageToGeneral(commandBuffer, g_context.reactive);
    transitionImage(
        commandBuffer,
        color,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );
    transitionImage(
        commandBuffer,
        depth,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );

    updateMotionDescriptors(colorView, depthView);
    g_context.vk.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_context.motionPipeline);
    g_context.vk.cmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_context.motionPipelineLayout,
        0,
        1,
        &g_context.descriptorSet,
        0,
        nullptr
    );
    g_context.vk.cmdPushConstants(
        commandBuffer,
        g_context.motionPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants
    );
    g_context.vk.cmdDispatch(commandBuffer, (g_context.renderWidth + 7) / 8, (g_context.renderHeight + 7) / 8, 1);
    transitionImage(
        commandBuffer,
        g_context.motion.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    );
    transitionImage(
        commandBuffer,
        g_context.reactive.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    );
}

bool recordFsrDispatch(
    VkCommandBuffer commandBuffer,
    VkImage color,
    VkImageView colorView,
    VkImage depth,
    VkImageView depthView,
    float jitterX,
    float jitterY,
    float frameTimeMillis,
    float sharpness,
    bool reset,
    float verticalFov
) {
    FfxFsr2DispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListVK(commandBuffer);
    dispatch.color = ffxGetTextureResourceVK(
        &g_context.fsr,
        color,
        colorView,
        g_context.renderWidth,
        g_context.renderHeight,
        VK_FORMAT_R8G8B8A8_UNORM,
        L"Minecraft world color",
        FFX_RESOURCE_STATE_COMPUTE_READ
    );
    dispatch.depth = ffxGetTextureResourceVK(
        &g_context.fsr,
        depth,
        depthView,
        g_context.renderWidth,
        g_context.renderHeight,
        VK_FORMAT_D32_SFLOAT,
        L"Minecraft world depth",
        FFX_RESOURCE_STATE_COMPUTE_READ
    );
    dispatch.motionVectors = ffxGetTextureResourceVK(
        &g_context.fsr,
        g_context.motion.image,
        g_context.motion.view,
        g_context.renderWidth,
        g_context.renderHeight,
        VK_FORMAT_R16G16_SFLOAT,
        L"MacFSRMC camera motion",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS
    );
    dispatch.reactive = ffxGetTextureResourceVK(
        &g_context.fsr,
        g_context.reactive.image,
        g_context.reactive.view,
        g_context.renderWidth,
        g_context.renderHeight,
        VK_FORMAT_R8_UNORM,
        L"MacFSRMC reactive mask",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS
    );
    dispatch.output = ffxGetTextureResourceVK(
        &g_context.fsr,
        g_context.output.image,
        g_context.output.view,
        g_context.displayWidth,
        g_context.displayHeight,
        VK_FORMAT_R8G8B8A8_UNORM,
        L"MacFSRMC FSR output",
        FFX_RESOURCE_STATE_UNORDERED_ACCESS
    );
    dispatch.jitterOffset = {jitterX, jitterY};
    dispatch.motionVectorScale = {static_cast<float>(g_context.renderWidth), static_cast<float>(g_context.renderHeight)};
    dispatch.renderSize = {g_context.renderWidth, g_context.renderHeight};
    dispatch.enableSharpening = sharpness > 0.0F;
    dispatch.sharpness = std::clamp(sharpness, 0.0F, 1.0F);
    dispatch.frameTimeDelta = std::clamp(frameTimeMillis, 1.0F, 1000.0F);
    dispatch.preExposure = 1.0F;
    dispatch.reset = reset;
    // Minecraft uses reversed-Z with an infinite far plane. FSR names the physical
    // near distance cameraFar for this projection convention.
    dispatch.cameraNear = FLT_MAX;
    dispatch.cameraFar = 0.05F;
    dispatch.cameraFovAngleVertical = std::clamp(verticalFov, 0.1F, 3.0F);
    dispatch.viewSpaceToMetersFactor = 1.0F;
    FfxErrorCode error = ffxFsr2ContextDispatch(&g_context.fsr, &dispatch);
    if (error != FFX_OK) {
        setError("ffxFsr2ContextDispatch failed with code " + std::to_string(error));
        return false;
    }
    return true;
}

void restoreAndBlit(
    VkCommandBuffer commandBuffer,
    VkImage color,
    VkImage depth,
    VkImage destination,
    uint32_t destinationWidth,
    uint32_t destinationHeight
) {
    transitionImage(
        commandBuffer,
        color,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    );
    transitionImage(
        commandBuffer,
        depth,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    );
    transitionImage(
        commandBuffer,
        g_context.motion.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    );
    transitionImage(
        commandBuffer,
        g_context.reactive.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    );
    transitionImage(
        commandBuffer,
        g_context.output.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT
    );
    transitionImage(
        commandBuffer,
        destination,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT
    );

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {static_cast<int32_t>(g_context.displayWidth), static_cast<int32_t>(g_context.displayHeight), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {static_cast<int32_t>(destinationWidth), static_cast<int32_t>(destinationHeight), 1};
    g_context.vk.cmdBlitImage(
        commandBuffer,
        g_context.output.image,
        VK_IMAGE_LAYOUT_GENERAL,
        destination,
        VK_IMAGE_LAYOUT_GENERAL,
        1,
        &region,
        VK_FILTER_LINEAR
    );
    transitionImage(
        commandBuffer,
        destination,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    );
}

void destroyBridge() {
    if (g_context.device != VK_NULL_HANDLE && g_context.vk.deviceWaitIdle != nullptr) {
        g_context.vk.deviceWaitIdle(g_context.device);
    }
    if (g_context.device != VK_NULL_HANDLE) {
        destroyFsrResources();
    }
    g_context = {};
    macfsr_unload_vulkan();
}
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_foreground_macfsrmc_client_Fsr2Native_initialize(
    JNIEnv*,
    jclass,
    jlong instanceHandle,
    jlong physicalDeviceHandle,
    jlong deviceHandle,
    jboolean debugLogging
) {
    std::lock_guard lock(g_mutex);
    VkInstance instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(instanceHandle));
    VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(physicalDeviceHandle));
    VkDevice device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(deviceHandle));
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        setError("A required Vulkan handle was null");
        return JNI_FALSE;
    }
    if (g_context.initialized
        && g_context.instance == instance
        && g_context.physicalDevice == physicalDevice
        && g_context.device == device) {
        g_context.debug = debugLogging == JNI_TRUE;
        return JNI_TRUE;
    }
    if (g_context.initialized) {
        destroyBridge();
    }

    g_context.instance = instance;
    g_context.physicalDevice = physicalDevice;
    g_context.device = device;
    g_context.debug = debugLogging == JNI_TRUE;
    if (!macfsr_load_vulkan(instance, device)) {
        setError(macfsr_vulkan_error());
        g_context = {};
        return JNI_FALSE;
    }
    if (!loadDeviceFunctions()) {
        destroyBridge();
        return JNI_FALSE;
    }
    g_context.initialized = true;
    g_lastError.clear();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL Java_com_foreground_macfsrmc_client_Fsr2Native_dispatch(
    JNIEnv* environment,
    jclass,
    jlong commandBufferHandle,
    jlong colorImageHandle,
    jlong colorViewHandle,
    jlong depthImageHandle,
    jlong depthViewHandle,
    jlong destinationImageHandle,
    jint inputWidth,
    jint inputHeight,
    jint outputWidth,
    jint outputHeight,
    jfloatArray currentToPreviousClip,
    jfloat jitterX,
    jfloat jitterY,
    jfloat frameTimeMillis,
    jfloat sharpness,
    jboolean reset,
    jboolean useReactiveMask,
    jfloat reactiveScale,
    jfloat verticalFov
) {
    std::lock_guard lock(g_mutex);
    if (!g_context.initialized) {
        setError("The native FSR 2 bridge is not initialized");
        return 0;
    }
    if (inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        setError("Invalid render dimensions passed to FSR 2");
        return 0;
    }
    if (currentToPreviousClip == nullptr || environment->GetArrayLength(currentToPreviousClip) < 16) {
        setError("The temporal reprojection matrix was missing or incomplete");
        return 0;
    }

    VkCommandBuffer commandBuffer = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(commandBufferHandle));
    VkImage color = reinterpret_cast<VkImage>(static_cast<uintptr_t>(colorImageHandle));
    VkImageView colorView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(colorViewHandle));
    VkImage depth = reinterpret_cast<VkImage>(static_cast<uintptr_t>(depthImageHandle));
    VkImageView depthView = reinterpret_cast<VkImageView>(static_cast<uintptr_t>(depthViewHandle));
    VkImage destination = reinterpret_cast<VkImage>(static_cast<uintptr_t>(destinationImageHandle));
    if (commandBuffer == VK_NULL_HANDLE || color == VK_NULL_HANDLE || colorView == VK_NULL_HANDLE
        || depth == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE || destination == VK_NULL_HANDLE) {
        setError("A required per-frame Vulkan handle was null");
        return 0;
    }

    if (!createFsrResources(inputWidth, inputHeight, outputWidth, outputHeight)) {
        return 0;
    }

    MotionPushConstants pushConstants{};
    environment->GetFloatArrayRegion(currentToPreviousClip, 0, 16, pushConstants.currentToPreviousClip);
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        setError("Could not read the temporal reprojection matrix");
        return 0;
    }
    pushConstants.inverseRenderSize[0] = 1.0F / static_cast<float>(inputWidth);
    pushConstants.inverseRenderSize[1] = 1.0F / static_cast<float>(inputHeight);
    pushConstants.reactiveScale = useReactiveMask == JNI_TRUE ? std::clamp(reactiveScale, 0.0F, 1.0F) : 0.0F;
    pushConstants.resetHistory = reset == JNI_TRUE ? 1u : 0u;

    recordMotionPass(commandBuffer, color, colorView, depth, depthView, pushConstants);
    if (!recordFsrDispatch(
        commandBuffer,
        color,
        colorView,
        depth,
        depthView,
        jitterX,
        jitterY,
        frameTimeMillis,
        sharpness,
        reset == JNI_TRUE,
        verticalFov
    )) {
        return 0;
    }
    restoreAndBlit(commandBuffer, color, depth, destination, outputWidth, outputHeight);
    g_lastError.clear();
    return 1;
}

extern "C" JNIEXPORT jstring JNICALL Java_com_foreground_macfsrmc_client_Fsr2Native_lastError(JNIEnv* environment, jclass) {
    std::lock_guard lock(g_mutex);
    return environment->NewStringUTF(g_lastError.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_foreground_macfsrmc_client_Fsr2Native_shutdown(JNIEnv*, jclass) {
    std::lock_guard lock(g_mutex);
    destroyBridge();
    g_lastError.clear();
}
