package com.foreground.macfsrmc.client;

import org.joml.Matrix4f;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class Fsr2TemporalHistoryTest {
    private static final Matrix4f PROJECTION = new Matrix4f().perspective(
        (float)Math.toRadians(70.0),
        16.0F / 9.0F,
        0.05F,
        1024.0F,
        true
    );
    private static final Matrix4f VIEW = new Matrix4f();

    @Test
    void matchesOfficialJitterPhaseFormula() {
        assertEquals(18, Fsr2TemporalHistory.jitterPhaseCount(1280, 1920));
        assertEquals(8, Fsr2TemporalHistory.jitterPhaseCount(1920, 1920));
        assertEquals(72, Fsr2TemporalHistory.jitterPhaseCount(640, 1920));
    }

    @Test
    void haltonSequenceMatchesFsrHelper() {
        assertEquals(0.5F, Fsr2TemporalHistory.halton(1, 2));
        assertEquals(0.25F, Fsr2TemporalHistory.halton(2, 2));
        assertEquals(1.0F / 3.0F, Fsr2TemporalHistory.halton(1, 3), 1.0E-6F);
    }

    @Test
    void promotesOnlyRecordedFramesAndResetsOnCameraCut() {
        Object world = new Object();
        Fsr2TemporalHistory history = new Fsr2TemporalHistory();
        history.beginFrame(world, 0.0, 64.0, 0.0, 1280, 720, 1920, 1_000_000_000L);
        history.jitterProjection(PROJECTION, VIEW);
        assertTrue(history.frame().reset());
        assertTrue(history.frame().jitteredProjection().isFinite());
        history.commit();

        history.beginFrame(world, 0.25, 64.0, 0.0, 1280, 720, 1920, 1_016_666_667L);
        history.jitterProjection(PROJECTION, VIEW);
        assertFalse(history.frame().reset());
        assertEquals(16.6667F, history.frame().frameTimeMillis(), 0.01F);
        history.commit();

        history.beginFrame(world, 100.0, 64.0, 0.0, 1280, 720, 1920, 1_033_333_334L);
        history.jitterProjection(PROJECTION, VIEW);
        assertTrue(history.frame().reset());
    }

    @Test
    void invalidationForcesHistoryReset() {
        Object world = new Object();
        Fsr2TemporalHistory history = new Fsr2TemporalHistory();
        history.beginFrame(world, 0.0, 0.0, 0.0, 960, 540, 1920, 10L);
        history.jitterProjection(PROJECTION, VIEW);
        history.commit();
        history.invalidate();

        history.beginFrame(world, 0.0, 0.0, 0.0, 960, 540, 1920, 20L);
        history.jitterProjection(PROJECTION, VIEW);
        assertTrue(history.frame().reset());
    }
}

