package com.foreground.macfsrmc.mixin.client;

import com.foreground.macfsrmc.client.Fsr2VulkanBootstrap;
import com.mojang.blaze3d.vulkan.VulkanBackend;
import com.mojang.blaze3d.vulkan.VulkanPhysicalDevice;
import com.mojang.blaze3d.vulkan.init.VulkanFeature;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArgs;
import org.spongepowered.asm.mixin.injection.invoke.arg.Args;

import java.util.Set;

@Mixin(VulkanBackend.class)
public abstract class VulkanBackendMixin {
    @ModifyArgs(
        method = "createDevice(JLcom/mojang/blaze3d/shaders/ShaderSource;Lcom/mojang/blaze3d/shaders/GpuDebugOptions;Ljava/lang/Runnable;)Lcom/mojang/blaze3d/systems/GpuDevice;",
        at = @At(
            value = "INVOKE",
            target = "Lcom/mojang/blaze3d/vulkan/VulkanBackend;createDevice(Ljava/util/Collection;Lcom/mojang/blaze3d/vulkan/VulkanPhysicalDevice;Ljava/util/Set;)Lorg/lwjgl/vulkan/VkDevice;"
        )
    )
    private void macfsrmc$enableFsr2DeviceFeatures(Args args) {
        VulkanPhysicalDevice physicalDevice = args.get(1);
        Set<VulkanFeature> enabledFeatures = args.get(2);
        Fsr2VulkanBootstrap.enableRequiredFeature(physicalDevice, enabledFeatures);
    }
}
