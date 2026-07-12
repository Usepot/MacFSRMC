package com.foreground.macfsrmc.client;

import com.foreground.macfsrmc.MacFsrMc;
import com.mojang.blaze3d.vulkan.VulkanBackend;
import com.mojang.blaze3d.vulkan.VulkanPhysicalDevice;
import com.mojang.blaze3d.vulkan.init.VulkanFeature;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.vulkan.VK12;
import org.lwjgl.vulkan.VkPhysicalDeviceFeatures;

import java.util.Set;

/**
 * Enables the optional core Vulkan feature required by FSR 2's storage-image
 * formats while Minecraft is assembling its logical-device feature set.
 */
public final class Fsr2VulkanBootstrap {
    private static final VulkanFeature SHADER_STORAGE_IMAGE_EXTENDED_FORMATS = new VulkanFeature(
        VulkanBackend.VK10_FEATURES_STRUCT,
        "shaderStorageImageExtendedFormats",
        VkPhysicalDeviceFeatures.SHADERSTORAGEIMAGEEXTENDEDFORMATS
    );

    private static volatile boolean enabled;

    private Fsr2VulkanBootstrap() {
    }

    public static void enableRequiredFeature(VulkanPhysicalDevice physicalDevice, Set<VulkanFeature> enabledFeatures) {
        boolean supported;
        try (MemoryStack stack = MemoryStack.stackPush()) {
            VkPhysicalDeviceFeatures supportedFeatures = VkPhysicalDeviceFeatures.calloc(stack);
            VK12.vkGetPhysicalDeviceFeatures(physicalDevice.vkPhysicalDevice(), supportedFeatures);
            supported = supportedFeatures.shaderStorageImageExtendedFormats();
        }

        if (!supported) {
            enabled = false;
            MacFsrMc.LOGGER.warn(
                "FSR 2 is unavailable on Vulkan device '{}': shaderStorageImageExtendedFormats is not supported",
                physicalDevice.deviceName()
            );
            return;
        }

        enabledFeatures.add(SHADER_STORAGE_IMAGE_EXTENDED_FORMATS);
        enabled = true;
        MacFsrMc.LOGGER.info(
            "Enabled Vulkan shaderStorageImageExtendedFormats for FSR 2 on '{}'",
            physicalDevice.deviceName()
        );
    }

    public static boolean isEnabled() {
        return enabled;
    }
}
