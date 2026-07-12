package com.foreground.macfsrmc.client;

import org.joml.Matrix4f;
import org.joml.Matrix4fc;

import java.util.Objects;

final class Fsr2Frame {
    private final boolean reset;
    private final float jitterX;
    private final float jitterY;
    private final float frameTimeMillis;
    private final float verticalFov;
    private final Matrix4f jitteredProjection;
    private final Matrix4f currentToPreviousClip;

    Fsr2Frame(
        boolean reset,
        float jitterX,
        float jitterY,
        float frameTimeMillis,
        float verticalFov,
        Matrix4fc jitteredProjection,
        Matrix4fc currentToPreviousClip
    ) {
        if (!Float.isFinite(jitterX) || !Float.isFinite(jitterY) || !Float.isFinite(frameTimeMillis) || !Float.isFinite(verticalFov)) {
            throw new IllegalArgumentException("Temporal frame values must be finite");
        }
        this.reset = reset;
        this.jitterX = jitterX;
        this.jitterY = jitterY;
        this.frameTimeMillis = Math.clamp(frameTimeMillis, 1.0F, 1000.0F);
        this.verticalFov = Math.clamp(verticalFov, 0.1F, 3.0F);
        this.jitteredProjection = finiteCopy(jitteredProjection, "jittered projection");
        this.currentToPreviousClip = finiteCopy(currentToPreviousClip, "current-to-previous clip matrix");
    }

    boolean reset() {
        return this.reset;
    }

    float jitterX() {
        return this.jitterX;
    }

    float jitterY() {
        return this.jitterY;
    }

    float frameTimeMillis() {
        return this.frameTimeMillis;
    }

    float verticalFov() {
        return this.verticalFov;
    }

    Matrix4f jitteredProjection() {
        return new Matrix4f(this.jitteredProjection);
    }

    float[] currentToPreviousClipArray() {
        return this.currentToPreviousClip.get(new float[16]);
    }

    private static Matrix4f finiteCopy(Matrix4fc matrix, String label) {
        Matrix4f copy = new Matrix4f(Objects.requireNonNull(matrix, label));
        if (!copy.isFinite()) {
            throw new IllegalArgumentException(label + " contains a non-finite value");
        }
        return copy;
    }
}
