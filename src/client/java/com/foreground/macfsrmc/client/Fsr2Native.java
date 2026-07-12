package com.foreground.macfsrmc.client;

final class Fsr2Native {
    private Fsr2Native() {
    }

    static native boolean initialize(long instance, long physicalDevice, long device, boolean debugLogging);

    static native int dispatch(
        long commandBuffer,
        long colorImage,
        long colorImageView,
        long depthImage,
        long depthImageView,
        long destinationImage,
        int inputWidth,
        int inputHeight,
        int outputWidth,
        int outputHeight,
        float[] currentToPreviousClip,
        float jitterX,
        float jitterY,
        float frameTimeMillis,
        float sharpness,
        boolean reset,
        boolean useReactiveMask,
        float reactiveScale,
        float verticalFov
    );

    static native String lastError();

    static native void shutdown();
}
