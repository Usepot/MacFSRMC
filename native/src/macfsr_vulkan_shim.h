#pragma once

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

bool macfsr_load_vulkan(VkInstance instance, VkDevice device);
void macfsr_unload_vulkan(void);
const char* macfsr_vulkan_error(void);

PFN_vkVoidFunction VKAPI_PTR macfsr_vkGetDeviceProcAddr(VkDevice device, const char* name);
VkResult VKAPI_PTR macfsr_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* layerName,
    uint32_t* propertyCount,
    VkExtensionProperties* properties
);
void VKAPI_PTR macfsr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* properties
);
void VKAPI_PTR macfsr_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* properties);
void VKAPI_PTR macfsr_vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* properties);
void VKAPI_PTR macfsr_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features);

#ifdef __cplusplus
}
#endif

