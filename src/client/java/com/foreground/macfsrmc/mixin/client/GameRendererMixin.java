package com.foreground.macfsrmc.mixin.client;

import com.foreground.macfsrmc.client.Fsr2Config;
import com.foreground.macfsrmc.client.Fsr2Controller;
import com.foreground.macfsrmc.client.MacFsrMcClient;
import com.mojang.blaze3d.GpuFormat;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.pipeline.TextureTarget;
import com.mojang.blaze3d.resource.CrossFrameResourcePool;
import com.mojang.blaze3d.vulkan.VulkanGpuTexture;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.GlobalSettingsUniform;
import net.minecraft.client.renderer.state.GameRenderState;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Mutable;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyArg;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
    @Shadow @Final @Mutable
    private RenderTarget mainRenderTarget;

    @Shadow @Final
    private Minecraft minecraft;

    @Shadow @Final
    private CrossFrameResourcePool resourcePool;

    @Shadow @Final
    private GameRenderState gameRenderState;

    @Shadow @Final
    private GlobalSettingsUniform globalSettingsUniform;

    @Unique
    private RenderTarget macfsrmc$displayTarget;

    @Unique
    private TextureTarget macfsrmc$worldTarget;

    @Unique
    private boolean macfsrmc$renderingScaledWorld;

    @Unique
    private boolean macfsrmc$levelTargetsAreScaled;

    @Inject(method = "<init>", at = @At("TAIL"))
    private void macfsrmc$rememberDisplayTarget(CallbackInfo callbackInfo) {
        this.macfsrmc$displayTarget = this.mainRenderTarget;
    }

    @Inject(
        method = "render",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/GameRenderer;renderLevel(Lnet/minecraft/client/DeltaTracker;)V",
            shift = At.Shift.BEFORE
        )
    )
    private void macfsrmc$beginScaledWorld(DeltaTracker deltaTracker, boolean advanceGameTime, CallbackInfo callbackInfo) {
        Fsr2Controller controller = MacFsrMcClient.controller();
        RenderTarget displayTarget = this.mainRenderTarget;
        this.macfsrmc$displayTarget = displayTarget;
        if (!(displayTarget.getColorTexture() instanceof VulkanGpuTexture)) {
            this.macfsrmc$restoreLevelTargets();
            return;
        }

        Fsr2Config config = controller.config();
        int inputWidth = config.scaledWidth(displayTarget.width);
        int inputHeight = config.scaledHeight(displayTarget.height);
        if (!controller.prepare(inputWidth, inputHeight, displayTarget.width, displayTarget.height)) {
            this.macfsrmc$restoreLevelTargets();
            return;
        }

        try {
            boolean targetChanged = false;
            if (this.macfsrmc$worldTarget == null) {
                this.macfsrmc$worldTarget = new TextureTarget(
                    "FSR 2 World",
                    inputWidth,
                    inputHeight,
                    true,
                    GpuFormat.RGBA8_UNORM
                );
                targetChanged = true;
            } else if (this.macfsrmc$worldTarget.width != inputWidth || this.macfsrmc$worldTarget.height != inputHeight) {
                this.macfsrmc$worldTarget.resize(inputWidth, inputHeight);
                targetChanged = true;
            }

            if (targetChanged) {
                this.resourcePool.clear();
                controller.invalidateHistory();
            }
            if (!this.macfsrmc$levelTargetsAreScaled || targetChanged) {
                this.minecraft.levelRenderer.resize(inputWidth, inputHeight);
                this.macfsrmc$levelTargetsAreScaled = true;
            }

            this.macfsrmc$updateGlobalSettings(deltaTracker, inputWidth, inputHeight);
            var camera = this.gameRenderState.levelRenderState.cameraRenderState;
            controller.beginFrame(
                this.minecraft.level,
                camera.pos.x,
                camera.pos.y,
                camera.pos.z,
                inputWidth,
                inputHeight,
                displayTarget.width
            );
            this.mainRenderTarget = this.macfsrmc$worldTarget;
            this.macfsrmc$renderingScaledWorld = true;
        } catch (Throwable throwable) {
            controller.fail(throwable);
            this.mainRenderTarget = displayTarget;
            this.macfsrmc$renderingScaledWorld = false;
            this.macfsrmc$restoreLevelTargets();
        }
    }

    @ModifyArg(
        method = "renderLevel",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/ProjectionMatrixBuffer;getBuffer(Lorg/joml/Matrix4f;)Lcom/mojang/blaze3d/buffers/GpuBufferSlice;"
        ),
        index = 0
    )
    private Matrix4f macfsrmc$jitterWorldProjection(Matrix4f projectionMatrix) {
        if (!this.macfsrmc$renderingScaledWorld) {
            return projectionMatrix;
        }
        return MacFsrMcClient.controller().jitterProjection(
            projectionMatrix,
            this.gameRenderState.levelRenderState.cameraRenderState.viewRotationMatrix
        );
    }

    @Inject(
        method = "renderLevel",
        at = @At(
            value = "INVOKE",
            target = "Lcom/mojang/blaze3d/systems/CommandEncoder;clearDepthTexture(Lcom/mojang/blaze3d/textures/GpuTexture;D)V",
            shift = At.Shift.BEFORE
        )
    )
    private void macfsrmc$upscaleWorldBeforeHand(DeltaTracker deltaTracker, CallbackInfo callbackInfo) {
        this.macfsrmc$finishScaledWorld(deltaTracker);
    }

    @Inject(
        method = "render",
        at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/renderer/fog/FogRenderer;endFrame()V",
            shift = At.Shift.BEFORE
        )
    )
    private void macfsrmc$upscaleWorldBeforeGui(DeltaTracker deltaTracker, boolean advanceGameTime, CallbackInfo callbackInfo) {
        this.macfsrmc$finishScaledWorld(deltaTracker);
    }

    @Inject(method = "resize", at = @At("TAIL"))
    private void macfsrmc$noticeVanillaResize(int width, int height, CallbackInfo callbackInfo) {
        this.macfsrmc$displayTarget = this.mainRenderTarget;
        this.macfsrmc$levelTargetsAreScaled = false;
        MacFsrMcClient.controller().invalidateHistory();
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void macfsrmc$releaseUpscalingResources(CallbackInfo callbackInfo) {
        if (this.macfsrmc$displayTarget != null) {
            this.mainRenderTarget = this.macfsrmc$displayTarget;
        }
        if (this.macfsrmc$worldTarget != null && this.macfsrmc$worldTarget != this.macfsrmc$displayTarget) {
            this.macfsrmc$worldTarget.destroyBuffers();
            this.macfsrmc$worldTarget = null;
        }
        MacFsrMcClient.closeController();
    }

    @Unique
    private void macfsrmc$finishScaledWorld(DeltaTracker deltaTracker) {
        if (!this.macfsrmc$renderingScaledWorld) {
            return;
        }

        RenderTarget scaledTarget = this.mainRenderTarget;
        RenderTarget displayTarget = this.macfsrmc$displayTarget;
        Fsr2Controller controller = MacFsrMcClient.controller();
        try {
            controller.upscale(scaledTarget, displayTarget);
        } finally {
            this.mainRenderTarget = displayTarget;
            this.macfsrmc$renderingScaledWorld = false;
            this.macfsrmc$updateGlobalSettings(deltaTracker, displayTarget.width, displayTarget.height);
        }

        if (controller.isPermanentlyDisabled()) {
            this.macfsrmc$restoreLevelTargets();
        }
    }

    @Unique
    private void macfsrmc$restoreLevelTargets() {
        if (!this.macfsrmc$levelTargetsAreScaled || this.macfsrmc$displayTarget == null || this.minecraft.levelRenderer == null) {
            return;
        }
        this.minecraft.levelRenderer.resize(this.macfsrmc$displayTarget.width, this.macfsrmc$displayTarget.height);
        this.resourcePool.clear();
        this.macfsrmc$levelTargetsAreScaled = false;
    }

    @Unique
    private void macfsrmc$updateGlobalSettings(DeltaTracker deltaTracker, int width, int height) {
        this.globalSettingsUniform.update(
            width,
            height,
            this.gameRenderState.optionsRenderState.glintStrength,
            this.minecraft.level == null ? 0L : this.minecraft.level.getGameTime(),
            deltaTracker,
            this.gameRenderState.optionsRenderState.menuBackgroundBlurriness,
            this.gameRenderState.levelRenderState.cameraRenderState.pos,
            this.gameRenderState.optionsRenderState.textureFiltering == net.minecraft.client.TextureFilteringMethod.RGSS
        );
    }
}
