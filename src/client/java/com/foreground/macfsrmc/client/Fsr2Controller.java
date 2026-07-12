package com.foreground.macfsrmc.client;

import com.foreground.macfsrmc.MacFsrMc;
import com.foreground.macfsrmc.mixin.client.GpuDeviceAccessor;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.GpuDeviceBackend;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.vulkan.VulkanCommandEncoder;
import com.mojang.blaze3d.vulkan.VulkanDevice;
import com.mojang.blaze3d.vulkan.VulkanGpuTexture;
import com.mojang.blaze3d.vulkan.VulkanGpuTextureView;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.vulkan.KHRSynchronization2;
import org.lwjgl.vulkan.VK12;
import org.lwjgl.vulkan.VkCommandBuffer;
import org.lwjgl.vulkan.VkDependencyInfo;
import org.lwjgl.vulkan.VkImageBlit;
import org.lwjgl.vulkan.VkImageMemoryBarrier2;
import org.lwjgl.vulkan.VkImageSubresourceLayers;
import org.lwjgl.vulkan.VkImageSubresourceRange;
import org.lwjgl.vulkan.VkOffset3D;

import java.util.Locale;

public final class Fsr2Controller implements AutoCloseable {
    private enum State {
        UNINITIALIZED,
        READY,
        UNSUPPORTED,
        FAILED,
        CLOSED
    }

    private static final int MAX_CONSECUTIVE_FAILURES = 3;

    private final Fsr2Config config;
    private final Fsr2TemporalHistory temporalHistory = new Fsr2TemporalHistory();
    private State state = State.UNINITIALIZED;
    private VulkanDevice vulkanDevice;
    private int consecutiveFailures;
    private boolean skipCurrentTemporalFrame;
    private boolean loggedSuccessfulDispatch;

    public Fsr2Controller(Fsr2Config config) {
        this.config = config;
    }

    public Fsr2Config config() {
        return this.config;
    }

    public boolean prepare(int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
        if (!this.config.enabled() || this.state == State.UNSUPPORTED || this.state == State.FAILED || this.state == State.CLOSED) {
            return false;
        }

        try {
            GpuDeviceBackend backend = ((GpuDeviceAccessor)(Object)RenderSystem.getDevice()).macfsrmc$getBackend();
            if (!(backend instanceof VulkanDevice activeVulkanDevice)) {
                if (this.state == State.UNINITIALIZED) {
                    MacFsrMc.LOGGER.warn(
                        "FSR 2 requires Minecraft's Vulkan renderer; active backend is {}",
                        RenderSystem.getDevice().getDeviceInfo().backendName()
                    );
                }
                this.state = State.UNSUPPORTED;
                return false;
            }

            if (!Fsr2VulkanBootstrap.isEnabled()) {
                if (this.state == State.UNINITIALIZED) {
                    MacFsrMc.LOGGER.warn(
                        "FSR 2 requires Vulkan shaderStorageImageExtendedFormats to be enabled during device creation"
                    );
                }
                this.state = State.UNSUPPORTED;
                return false;
            }

            if (this.vulkanDevice != activeVulkanDevice || this.state != State.READY) {
                NativeLibraryLoader.load();
                long instance = activeVulkanDevice.instance().vkInstance().address();
                long device = activeVulkanDevice.vkDevice().address();
                long physicalDevice = activeVulkanDevice.vkDevice().getPhysicalDevice().address();
                if (!Fsr2Native.initialize(instance, physicalDevice, device, this.config.debugLogging())) {
                    throw new IllegalStateException("Native initialization failed: " + Fsr2Native.lastError());
                }
                this.vulkanDevice = activeVulkanDevice;
                this.state = State.READY;
                this.temporalHistory.invalidate();
                this.loggedSuccessfulDispatch = false;
                MacFsrMc.LOGGER.info(
                    "FSR 2.2.1 ready: {}x{} -> {}x{} ({} mode, RCAS={})",
                    inputWidth,
                    inputHeight,
                    outputWidth,
                    outputHeight,
                    this.config.qualityMode().displayName(),
                    this.config.sharpness()
                );
            }
            return true;
        } catch (Throwable throwable) {
            this.disableAfterFailure("FSR 2 initialization failed", throwable);
            return false;
        }
    }

    public void beginFrame(
        Object worldToken,
        double cameraX,
        double cameraY,
        double cameraZ,
        int inputWidth,
        int inputHeight,
        int outputWidth
    ) {
        this.skipCurrentTemporalFrame = false;
        this.temporalHistory.beginFrame(
            worldToken,
            cameraX,
            cameraY,
            cameraZ,
            inputWidth,
            inputHeight,
            outputWidth,
            System.nanoTime()
        );
    }

    public Matrix4f jitterProjection(Matrix4fc baseProjection, Matrix4fc viewRotation) {
        Matrix4f original = new Matrix4f(baseProjection);
        if (!original.isFinite() || !new Matrix4f(viewRotation).isFinite()) {
            this.skipCurrentTemporalFrame = true;
            this.temporalHistory.invalidate();
            MacFsrMc.LOGGER.debug("Skipping FSR 2 temporal history for a bootstrap frame with an invalid camera matrix");
            return original;
        }

        try {
            return this.temporalHistory.jitterProjection(original, viewRotation);
        } catch (IllegalArgumentException exception) {
            this.skipCurrentTemporalFrame = true;
            this.temporalHistory.invalidate();
            MacFsrMc.LOGGER.debug("Skipping FSR 2 temporal history for a non-invertible camera frame", exception);
            return original;
        }
    }

    public boolean upscale(RenderTarget input, RenderTarget output) {
        if (this.state != State.READY || this.vulkanDevice == null) {
            return false;
        }

        if (!(input.getColorTexture() instanceof VulkanGpuTexture color)
            || !(input.getColorTextureView() instanceof VulkanGpuTextureView colorView)
            || !(input.getDepthTexture() instanceof VulkanGpuTexture depth)
            || !(input.getDepthTextureView() instanceof VulkanGpuTextureView depthView)
            || !(output.getColorTexture() instanceof VulkanGpuTexture destination)) {
            this.disableAfterFailure("FSR 2 received non-Vulkan render targets", null);
            return false;
        }

        VulkanCommandEncoder encoder = this.vulkanDevice.createCommandEncoder();
        VkCommandBuffer commandBuffer = encoder.allocateAndBeginTransientCommandBuffer();
        boolean recordedFsr = false;
        boolean recordedNativeFallback = false;
        boolean skippedTemporalFrame = this.skipCurrentTemporalFrame;
        try {
            if (!skippedTemporalFrame) {
                Fsr2Frame frame = this.temporalHistory.frame();
                int result = Fsr2Native.dispatch(
                    commandBuffer.address(),
                    color.vkImage(),
                    colorView.vkImageView(),
                    depth.vkImage(),
                    depthView.vkImageView(),
                    destination.vkImage(),
                    input.width,
                    input.height,
                    output.width,
                    output.height,
                    frame.currentToPreviousClipArray(),
                    frame.jitterX(),
                    frame.jitterY(),
                    frame.frameTimeMillis(),
                    this.config.sharpness(),
                    frame.reset(),
                    this.config.reactiveMask(),
                    this.config.reactiveScale(),
                    frame.verticalFov()
                );
                recordedFsr = result == 1;
                recordedNativeFallback = result == 2;
            }
            if (!recordedFsr && !recordedNativeFallback) {
                recordFallbackBlit(commandBuffer, color.vkImage(), destination.vkImage(), input.width, input.height, output.width, output.height);
            }

            int endResult = VK12.vkEndCommandBuffer(commandBuffer);
            if (endResult != VK12.VK_SUCCESS) {
                throw new IllegalStateException("vkEndCommandBuffer failed: " + endResult);
            }
            encoder.execute(commandBuffer);

            if (recordedFsr) {
                this.consecutiveFailures = 0;
                this.temporalHistory.commit();
                if (!this.loggedSuccessfulDispatch) {
                    this.loggedSuccessfulDispatch = true;
                    MacFsrMc.LOGGER.info(
                        "FSR 2 temporal reconstruction active: official Vulkan pass chain dispatched at {}x{} -> {}x{}",
                        input.width,
                        input.height,
                        output.width,
                        output.height
                    );
                }
                return true;
            }

            this.temporalHistory.invalidate();
            this.skipCurrentTemporalFrame = false;
            if (skippedTemporalFrame) {
                return false;
            }
            this.consecutiveFailures++;
            String nativeError = Fsr2Native.lastError();
            MacFsrMc.LOGGER.warn("FSR 2 frame fell back to a linear Vulkan blit: {}", nativeError);
            if (this.consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
                this.disableAfterFailure("FSR 2 failed repeatedly: " + nativeError, null);
            }
            return false;
        } catch (Throwable throwable) {
            this.temporalHistory.invalidate();
            this.disableAfterFailure("FSR 2 frame recording failed", throwable);
            return false;
        }
    }

    public boolean isPermanentlyDisabled() {
        return this.state == State.UNSUPPORTED || this.state == State.FAILED || this.state == State.CLOSED;
    }

    public void invalidateHistory() {
        this.temporalHistory.invalidate();
    }

    public void fail(Throwable throwable) {
        this.temporalHistory.invalidate();
        this.disableAfterFailure("FSR 2 integration failed", throwable);
    }

    private void disableAfterFailure(String message, Throwable throwable) {
        if (this.state != State.FAILED && this.state != State.CLOSED) {
            if (throwable == null) {
                MacFsrMc.LOGGER.error(message);
            } else {
                MacFsrMc.LOGGER.error(message, throwable);
            }
        }
        this.state = State.FAILED;
        this.temporalHistory.invalidate();
    }

    private static void recordFallbackBlit(
        VkCommandBuffer commandBuffer,
        long source,
        long destination,
        int sourceWidth,
        int sourceHeight,
        int destinationWidth,
        int destinationHeight
    ) {
        try (MemoryStack stack = MemoryStack.stackPush()) {
            VkImageMemoryBarrier2.Buffer barriers = VkImageMemoryBarrier2.calloc(2, stack);
            barriers.get(0)
                .sType$Default()
                .srcStageMask(65_536L) // VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                .srcAccessMask(65_536L) // VK_ACCESS_2_MEMORY_WRITE_BIT
                .dstStageMask(4_096L) // VK_PIPELINE_STAGE_2_TRANSFER_BIT
                .dstAccessMask(2_048L) // VK_ACCESS_2_TRANSFER_READ_BIT
                .oldLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .newLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .srcQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .dstQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .image(source);
            setColorRange(barriers.get(0).subresourceRange());
            barriers.get(1)
                .sType$Default()
                .srcStageMask(65_536L)
                .srcAccessMask(65_536L)
                .dstStageMask(4_096L)
                .dstAccessMask(4_096L) // VK_ACCESS_2_TRANSFER_WRITE_BIT
                .oldLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .newLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .srcQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .dstQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .image(destination);
            setColorRange(barriers.get(1).subresourceRange());
            KHRSynchronization2.vkCmdPipelineBarrier2KHR(
                commandBuffer,
                VkDependencyInfo.calloc(stack).sType$Default().pImageMemoryBarriers(barriers)
            );

            VkImageSubresourceLayers srcLayers = VkImageSubresourceLayers.calloc(stack)
                .aspectMask(VK12.VK_IMAGE_ASPECT_COLOR_BIT)
                .mipLevel(0)
                .baseArrayLayer(0)
                .layerCount(1);
            VkImageSubresourceLayers dstLayers = VkImageSubresourceLayers.calloc(stack)
                .aspectMask(VK12.VK_IMAGE_ASPECT_COLOR_BIT)
                .mipLevel(0)
                .baseArrayLayer(0)
                .layerCount(1);
            VkOffset3D.Buffer sourceOffsets = VkOffset3D.calloc(2, stack);
            sourceOffsets.get(0).set(0, 0, 0);
            sourceOffsets.get(1).set(sourceWidth, sourceHeight, 1);
            VkOffset3D.Buffer destinationOffsets = VkOffset3D.calloc(2, stack);
            destinationOffsets.get(0).set(0, 0, 0);
            destinationOffsets.get(1).set(destinationWidth, destinationHeight, 1);
            VkImageBlit.Buffer region = VkImageBlit.calloc(1, stack);
            region.get(0)
                .srcSubresource(srcLayers)
                .srcOffsets(sourceOffsets)
                .dstSubresource(dstLayers)
                .dstOffsets(destinationOffsets);
            VK12.vkCmdBlitImage(
                commandBuffer,
                source,
                VK12.VK_IMAGE_LAYOUT_GENERAL,
                destination,
                VK12.VK_IMAGE_LAYOUT_GENERAL,
                region,
                VK12.VK_FILTER_LINEAR
            );

            VkImageMemoryBarrier2.Buffer postBlitBarrier = VkImageMemoryBarrier2.calloc(1, stack);
            postBlitBarrier.get(0)
                .sType$Default()
                .srcStageMask(4_096L) // VK_PIPELINE_STAGE_2_TRANSFER_BIT
                .srcAccessMask(4_096L) // VK_ACCESS_2_TRANSFER_WRITE_BIT
                .dstStageMask(65_536L) // VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                .dstAccessMask(32_768L | 65_536L) // VK_ACCESS_2_MEMORY_READ/WRITE_BIT
                .oldLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .newLayout(VK12.VK_IMAGE_LAYOUT_GENERAL)
                .srcQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .dstQueueFamilyIndex(VK12.VK_QUEUE_FAMILY_IGNORED)
                .image(destination);
            setColorRange(postBlitBarrier.get(0).subresourceRange());
            KHRSynchronization2.vkCmdPipelineBarrier2KHR(
                commandBuffer,
                VkDependencyInfo.calloc(stack).sType$Default().pImageMemoryBarriers(postBlitBarrier)
            );
        }
    }

    private static void setColorRange(VkImageSubresourceRange range) {
        range.aspectMask(VK12.VK_IMAGE_ASPECT_COLOR_BIT)
            .baseMipLevel(0)
            .levelCount(1)
            .baseArrayLayer(0)
            .layerCount(1);
    }

    @Override
    public void close() {
        if (this.state == State.CLOSED) {
            return;
        }
        this.state = State.CLOSED;
        this.temporalHistory.invalidate();
        if (this.vulkanDevice != null) {
            try {
                this.vulkanDevice.graphicsQueue().waitIdle();
                Fsr2Native.shutdown();
            } catch (Throwable throwable) {
                MacFsrMc.LOGGER.warn("FSR 2 shutdown failed on {}", System.getProperty("os.name", "unknown").toLowerCase(Locale.ROOT), throwable);
            }
            this.vulkanDevice = null;
        }
    }
}
