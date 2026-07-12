#include "macfsr_vulkan_shim.h"

#include <array>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_enumerateDeviceExtensionProperties = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_getPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties g_getPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 g_getPhysicalDeviceProperties2 = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_getPhysicalDeviceFeatures2 = nullptr;
std::string g_error;

#if defined(_WIN32)
HMODULE g_library = nullptr;
bool g_ownsLibrary = false;

void* findSymbol(const char* name) {
    return g_library == nullptr ? nullptr : reinterpret_cast<void*>(GetProcAddress(g_library, name));
}
#else
void* g_library = nullptr;
bool g_ownsLibrary = false;

void* findSymbol(const char* name) {
    void* symbol = dlsym(RTLD_DEFAULT, name);
    return symbol != nullptr || g_library == nullptr ? symbol : dlsym(g_library, name);
}
#endif

template <typename T>
T instanceFunction(VkInstance instance, const char* name) {
    if (g_getInstanceProcAddr != nullptr) {
        if (PFN_vkVoidFunction function = g_getInstanceProcAddr(instance, name)) {
            return reinterpret_cast<T>(function);
        }
    }
    return reinterpret_cast<T>(findSymbol(name));
}

void clearFunctions() {
    g_getInstanceProcAddr = nullptr;
    g_getDeviceProcAddr = nullptr;
    g_enumerateDeviceExtensionProperties = nullptr;
    g_getPhysicalDeviceMemoryProperties = nullptr;
    g_getPhysicalDeviceProperties = nullptr;
    g_getPhysicalDeviceProperties2 = nullptr;
    g_getPhysicalDeviceFeatures2 = nullptr;
}
}

extern "C" bool macfsr_load_vulkan(VkInstance instance, VkDevice device) {
    if (g_getDeviceProcAddr != nullptr) {
        return true;
    }

#if defined(_WIN32)
    g_library = GetModuleHandleW(L"vulkan-1.dll");
    if (g_library == nullptr) {
        g_library = LoadLibraryW(L"vulkan-1.dll");
        g_ownsLibrary = g_library != nullptr;
    }
#else
    constexpr std::array<const char*, 6> candidates = {
#if defined(__APPLE__)
        "libMoltenVK.dylib",
        "libvulkan.1.dylib",
        "libvulkan.dylib",
        "@rpath/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
        "/opt/homebrew/lib/libMoltenVK.dylib"
#else
        "libvulkan.so.1",
        "libvulkan.so",
        "libMoltenVK.so",
        "/usr/lib/libvulkan.so.1",
        "/usr/local/lib/libvulkan.so.1",
        ""
#endif
    };
    if (dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr") == nullptr) {
        for (const char* candidate : candidates) {
            if (candidate[0] == '\0') {
                continue;
            }
            g_library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
            if (g_library != nullptr) {
                g_ownsLibrary = true;
                break;
            }
        }
    }
#endif

    g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(findSymbol("vkGetInstanceProcAddr"));
    if (g_getInstanceProcAddr == nullptr) {
        g_error = "Could not resolve vkGetInstanceProcAddr from Minecraft's Vulkan loader";
        macfsr_unload_vulkan();
        return false;
    }

    g_getDeviceProcAddr = instanceFunction<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
    g_enumerateDeviceExtensionProperties = instanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(
        instance,
        "vkEnumerateDeviceExtensionProperties"
    );
    g_getPhysicalDeviceMemoryProperties = instanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(
        instance,
        "vkGetPhysicalDeviceMemoryProperties"
    );
    g_getPhysicalDeviceProperties = instanceFunction<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
    g_getPhysicalDeviceProperties2 = instanceFunction<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    g_getPhysicalDeviceFeatures2 = instanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");

    if (g_getDeviceProcAddr == nullptr
        || g_enumerateDeviceExtensionProperties == nullptr
        || g_getPhysicalDeviceMemoryProperties == nullptr
        || g_getPhysicalDeviceProperties == nullptr
        || g_getPhysicalDeviceProperties2 == nullptr
        || g_getPhysicalDeviceFeatures2 == nullptr
        || device == VK_NULL_HANDLE) {
        g_error = "Minecraft's Vulkan loader is missing a required Vulkan 1.2 entry point";
        macfsr_unload_vulkan();
        return false;
    }

    g_error.clear();
    return true;
}

extern "C" void macfsr_unload_vulkan() {
    clearFunctions();
#if defined(_WIN32)
    if (g_ownsLibrary && g_library != nullptr) {
        FreeLibrary(g_library);
    }
#else
    if (g_ownsLibrary && g_library != nullptr) {
        dlclose(g_library);
    }
#endif
    g_library = nullptr;
    g_ownsLibrary = false;
}

extern "C" const char* macfsr_vulkan_error() {
    return g_error.c_str();
}

extern "C" PFN_vkVoidFunction VKAPI_PTR macfsr_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    return g_getDeviceProcAddr == nullptr ? nullptr : g_getDeviceProcAddr(device, name);
}

extern "C" VkResult VKAPI_PTR macfsr_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* layerName,
    uint32_t* propertyCount,
    VkExtensionProperties* properties
) {
    return g_enumerateDeviceExtensionProperties == nullptr
        ? VK_ERROR_INITIALIZATION_FAILED
        : g_enumerateDeviceExtensionProperties(physicalDevice, layerName, propertyCount, properties);
}

extern "C" void VKAPI_PTR macfsr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* properties
) {
    g_getPhysicalDeviceMemoryProperties(physicalDevice, properties);
}

extern "C" void VKAPI_PTR macfsr_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* properties
) {
    g_getPhysicalDeviceProperties(physicalDevice, properties);
}

extern "C" void VKAPI_PTR macfsr_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* properties
) {
    g_getPhysicalDeviceProperties2(physicalDevice, properties);
}

extern "C" void VKAPI_PTR macfsr_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* features
) {
    g_getPhysicalDeviceFeatures2(physicalDevice, features);
}

