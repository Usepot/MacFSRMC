package com.foreground.macfsrmc.client;

import org.joml.Matrix4f;
import org.joml.Matrix4fc;

import java.util.Arrays;
import java.util.Objects;

/**
 * Owns FSR 2's sub-pixel jitter sequence and camera reprojection history.
 * A pending frame is promoted only after its Vulkan commands were recorded.
 */
final class Fsr2TemporalHistory {
    private static final long MAX_FRAME_GAP_NANOS = 1_000_000_000L;
    private static final double MAX_CAMERA_JUMP_SQUARED = 32.0 * 32.0;
    private static final float PROJECTION_DISCONTINUITY_THRESHOLD = 0.25F;
    private static final float MIN_INVERTIBLE_DETERMINANT = 1.0E-8F;

    private boolean invalidated = true;
    private boolean hasCommittedFrame;
    private Object committedWorldToken;
    private double committedCameraX;
    private double committedCameraY;
    private double committedCameraZ;
    private int committedRenderWidth;
    private int committedRenderHeight;
    private int committedDisplayWidth;
    private long committedTimeNanos;
    private int nextJitterIndex;
    private Matrix4f committedBaseProjection;
    private Matrix4f committedJitteredProjectionView;

    private PendingFrame pending;
    private Fsr2Frame currentFrame;

    void beginFrame(
        Object worldToken,
        double cameraX,
        double cameraY,
        double cameraZ,
        int renderWidth,
        int renderHeight,
        int displayWidth,
        long nowNanos
    ) {
        if (renderWidth <= 0 || renderHeight <= 0 || displayWidth <= 0) {
            throw new IllegalArgumentException("Temporal frame dimensions must be positive");
        }
        if (!Double.isFinite(cameraX) || !Double.isFinite(cameraY) || !Double.isFinite(cameraZ)) {
            throw new IllegalArgumentException("Temporal camera position must be finite");
        }

        boolean reset = !this.hasCommittedFrame || this.invalidated || this.pending != null;
        float frameTimeMillis = 16.6667F;
        if (this.hasCommittedFrame) {
            reset |= worldToken != this.committedWorldToken;
            reset |= renderWidth != this.committedRenderWidth || renderHeight != this.committedRenderHeight;
            reset |= displayWidth != this.committedDisplayWidth;

            long elapsed = nowNanos - this.committedTimeNanos;
            reset |= elapsed <= 0L || elapsed > MAX_FRAME_GAP_NANOS;
            if (elapsed > 0L) {
                frameTimeMillis = elapsed / 1_000_000.0F;
            }

            double dx = cameraX - this.committedCameraX;
            double dy = cameraY - this.committedCameraY;
            double dz = cameraZ - this.committedCameraZ;
            double distanceSquared = dx * dx + dy * dy + dz * dz;
            reset |= !Double.isFinite(distanceSquared) || distanceSquared > MAX_CAMERA_JUMP_SQUARED;
        }

        int phaseCount = jitterPhaseCount(renderWidth, displayWidth);
        int jitterIndex = reset ? 0 : this.nextJitterIndex % phaseCount;
        float jitterX = halton(jitterIndex + 1, 2) - 0.5F;
        float jitterY = halton(jitterIndex + 1, 3) - 0.5F;
        this.pending = new PendingFrame(
            worldToken,
            cameraX,
            cameraY,
            cameraZ,
            renderWidth,
            renderHeight,
            displayWidth,
            nowNanos,
            frameTimeMillis,
            reset,
            jitterIndex,
            phaseCount,
            jitterX,
            jitterY
        );
        this.currentFrame = null;
    }

    Matrix4f jitterProjection(Matrix4fc baseProjection, Matrix4fc viewRotation) {
        PendingFrame active = this.requirePending();
        if (this.currentFrame != null) {
            throw new IllegalStateException("Projection jitter was already built for this frame");
        }

        Matrix4f base = finiteCopy(baseProjection, "base projection");
        Matrix4f view = finiteCopy(viewRotation, "view rotation");
        if (!active.reset && (this.committedBaseProjection == null
            || this.committedJitteredProjectionView == null
            || hasLargeProjectionDiscontinuity(base, this.committedBaseProjection))) {
            active.forceReset();
        }

        // Matches AMD's Vulkan sample: +X and vertically inverted Y in clip space.
        Matrix4f jitteredProjection = new Matrix4f(base);
        jitteredProjection.m20(jitteredProjection.m20() + 2.0F * active.jitterX / active.renderWidth);
        jitteredProjection.m21(jitteredProjection.m21() - 2.0F * active.jitterY / active.renderHeight);

        Matrix4f currentProjectionView = new Matrix4f(jitteredProjection).mul(view);
        Matrix4f inverseCurrent = checkedInverse(currentProjectionView, "current projection-view");
        Matrix4f currentToPrevious;
        if (active.reset) {
            currentToPrevious = new Matrix4f();
        } else {
            float relativeX = (float)(active.cameraX - this.committedCameraX);
            float relativeY = (float)(active.cameraY - this.committedCameraY);
            float relativeZ = (float)(active.cameraZ - this.committedCameraZ);
            Matrix4f previousFromCurrentCamera = new Matrix4f(this.committedJitteredProjectionView)
                .translate(relativeX, relativeY, relativeZ);
            currentToPrevious = previousFromCurrentCamera.mul(inverseCurrent);
            if (!currentToPrevious.isFinite()) {
                this.failActiveFrame("Temporal reprojection produced a non-finite matrix");
            }
        }

        active.baseProjection = new Matrix4f(base);
        active.jitteredProjectionView = new Matrix4f(currentProjectionView);
        float verticalFov = 2.0F * (float)Math.atan(1.0F / Math.abs(base.m11()));
        this.currentFrame = new Fsr2Frame(
            active.reset,
            active.jitterX,
            active.jitterY,
            active.frameTimeMillis,
            verticalFov,
            jitteredProjection,
            currentToPrevious
        );
        return new Matrix4f(jitteredProjection);
    }

    Fsr2Frame frame() {
        this.requirePending();
        if (this.currentFrame == null) {
            throw new IllegalStateException("jitterProjection must run before requesting the temporal frame");
        }
        return this.currentFrame;
    }

    void commit() {
        PendingFrame active = this.requirePending();
        if (this.currentFrame == null || active.baseProjection == null || active.jitteredProjectionView == null) {
            throw new IllegalStateException("A temporal frame cannot be committed before projection jitter");
        }
        this.committedWorldToken = active.worldToken;
        this.committedCameraX = active.cameraX;
        this.committedCameraY = active.cameraY;
        this.committedCameraZ = active.cameraZ;
        this.committedRenderWidth = active.renderWidth;
        this.committedRenderHeight = active.renderHeight;
        this.committedDisplayWidth = active.displayWidth;
        this.committedTimeNanos = active.nowNanos;
        this.committedBaseProjection = new Matrix4f(active.baseProjection);
        this.committedJitteredProjectionView = new Matrix4f(active.jitteredProjectionView);
        this.hasCommittedFrame = true;
        this.invalidated = false;
        this.nextJitterIndex = (active.jitterIndex + 1) % active.phaseCount;
        this.pending = null;
        this.currentFrame = null;
    }

    void invalidate() {
        this.invalidated = true;
        this.pending = null;
        this.currentFrame = null;
    }

    static int jitterPhaseCount(int renderWidth, int displayWidth) {
        float ratio = (float)displayWidth / renderWidth;
        return Math.max(1, (int)(8.0F * ratio * ratio));
    }

    static float halton(int index, int base) {
        float result = 0.0F;
        float fraction = 1.0F;
        for (int remaining = index; remaining > 0; remaining /= base) {
            fraction /= base;
            result += fraction * (remaining % base);
        }
        return result;
    }

    private PendingFrame requirePending() {
        if (this.pending == null) {
            throw new IllegalStateException("beginFrame must run before temporal history is used");
        }
        return this.pending;
    }

    private void failActiveFrame(String message) {
        this.invalidate();
        throw new IllegalArgumentException(message);
    }

    private Matrix4f checkedInverse(Matrix4fc matrix, String label) {
        float determinant = matrix.determinant();
        if (!Float.isFinite(determinant) || Math.abs(determinant) <= MIN_INVERTIBLE_DETERMINANT) {
            this.failActiveFrame(label + " is singular");
        }
        Matrix4f inverse = matrix.invert(new Matrix4f());
        if (!inverse.isFinite()) {
            this.failActiveFrame(label + " could not be inverted");
        }
        return inverse;
    }

    private static Matrix4f finiteCopy(Matrix4fc matrix, String label) {
        Matrix4f copy = new Matrix4f(Objects.requireNonNull(matrix, label));
        if (!copy.isFinite()) {
            throw new IllegalArgumentException(
                label + " contains a non-finite value: " + Arrays.toString(copy.get(new float[16]))
            );
        }
        return copy;
    }

    private static boolean hasLargeProjectionDiscontinuity(Matrix4fc current, Matrix4fc previous) {
        float[] currentValues = current.get(new float[16]);
        float[] previousValues = previous.get(new float[16]);
        float maximumDifference = 0.0F;
        float maximumReference = 1.0F;
        for (int index = 0; index < currentValues.length; index++) {
            maximumDifference = Math.max(maximumDifference, Math.abs(currentValues[index] - previousValues[index]));
            maximumReference = Math.max(maximumReference, Math.abs(previousValues[index]));
        }
        return maximumDifference > maximumReference * PROJECTION_DISCONTINUITY_THRESHOLD;
    }

    private static final class PendingFrame {
        private final Object worldToken;
        private final double cameraX;
        private final double cameraY;
        private final double cameraZ;
        private final int renderWidth;
        private final int renderHeight;
        private final int displayWidth;
        private final long nowNanos;
        private final float frameTimeMillis;
        private boolean reset;
        private int jitterIndex;
        private final int phaseCount;
        private float jitterX;
        private float jitterY;
        private Matrix4f baseProjection;
        private Matrix4f jitteredProjectionView;

        private PendingFrame(
            Object worldToken,
            double cameraX,
            double cameraY,
            double cameraZ,
            int renderWidth,
            int renderHeight,
            int displayWidth,
            long nowNanos,
            float frameTimeMillis,
            boolean reset,
            int jitterIndex,
            int phaseCount,
            float jitterX,
            float jitterY
        ) {
            this.worldToken = worldToken;
            this.cameraX = cameraX;
            this.cameraY = cameraY;
            this.cameraZ = cameraZ;
            this.renderWidth = renderWidth;
            this.renderHeight = renderHeight;
            this.displayWidth = displayWidth;
            this.nowNanos = nowNanos;
            this.frameTimeMillis = frameTimeMillis;
            this.reset = reset;
            this.jitterIndex = jitterIndex;
            this.phaseCount = phaseCount;
            this.jitterX = jitterX;
            this.jitterY = jitterY;
        }

        private void forceReset() {
            this.reset = true;
            this.jitterIndex = 0;
            this.jitterX = halton(1, 2) - 0.5F;
            this.jitterY = halton(1, 3) - 0.5F;
        }
    }
}
