package com.foreground.macfsrmc.client;

import com.foreground.macfsrmc.MacFsrMc;
import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import net.fabricmc.loader.api.FabricLoader;

import java.io.IOException;
import java.io.Reader;
import java.io.Writer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

public final class Fsr2Config {
    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static final Path PATH = FabricLoader.getInstance().getConfigDir().resolve("macfsrmc.json");

    private boolean enabled = true;
    private QualityMode qualityMode = QualityMode.QUALITY;
    private float sharpness = 0.2F;
    private boolean reactiveMask = true;
    private float reactiveScale = 0.65F;
    private boolean debugLogging = false;

    public static Fsr2Config load() {
        if (!Files.isRegularFile(PATH)) {
            Fsr2Config config = new Fsr2Config();
            config.save();
            return config;
        }

        try (Reader reader = Files.newBufferedReader(PATH)) {
            Fsr2Config config = GSON.fromJson(reader, Fsr2Config.class);
            if (config == null) {
                throw new IOException("configuration was empty");
            }
            config.validate();
            return config;
        } catch (Exception exception) {
            MacFsrMc.LOGGER.warn("Could not read {}; using defaults", PATH, exception);
            return new Fsr2Config();
        }
    }

    public synchronized void save() {
        this.validate();
        try {
            Files.createDirectories(PATH.getParent());
            Path temporary = PATH.resolveSibling(PATH.getFileName() + ".tmp");
            try (Writer writer = Files.newBufferedWriter(temporary)) {
                GSON.toJson(this, writer);
            }
            try {
                Files.move(temporary, PATH, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (IOException ignored) {
                Files.move(temporary, PATH, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException exception) {
            MacFsrMc.LOGGER.warn("Could not save {}", PATH, exception);
        }
    }

    private void validate() {
        if (this.qualityMode == null) {
            this.qualityMode = QualityMode.QUALITY;
        }
        if (!Float.isFinite(this.sharpness)) {
            this.sharpness = 0.2F;
        }
        if (!Float.isFinite(this.reactiveScale)) {
            this.reactiveScale = 0.65F;
        }
        this.sharpness = Math.clamp(this.sharpness, 0.0F, 1.0F);
        this.reactiveScale = Math.clamp(this.reactiveScale, 0.0F, 1.0F);
    }

    public synchronized boolean enabled() {
        return this.enabled;
    }

    public synchronized QualityMode qualityMode() {
        return this.qualityMode;
    }

    public synchronized float sharpness() {
        return this.sharpness;
    }

    public synchronized boolean reactiveMask() {
        return this.reactiveMask;
    }

    public synchronized float reactiveScale() {
        return this.reactiveScale;
    }

    public synchronized boolean debugLogging() {
        return this.debugLogging;
    }

    public synchronized int scaledWidth(int displayWidth) {
        return alignedDimension(displayWidth, this.qualityMode.renderScale());
    }

    public synchronized int scaledHeight(int displayHeight) {
        return alignedDimension(displayHeight, this.qualityMode.renderScale());
    }

    public synchronized void toggle() {
        this.enabled = !this.enabled;
        this.save();
    }

    public synchronized void cycleQuality() {
        QualityMode[] values = QualityMode.values();
        this.qualityMode = values[(this.qualityMode.ordinal() + 1) % values.length];
        this.save();
    }

    private static int alignedDimension(int displayDimension, float scale) {
        int scaled = Math.max(64, Math.round(displayDimension * scale));
        return Math.max(2, scaled & ~1);
    }

    public enum QualityMode {
        QUALITY(1.0F / 1.5F),
        BALANCED(1.0F / 1.7F),
        PERFORMANCE(1.0F / 2.0F),
        ULTRA_PERFORMANCE(1.0F / 3.0F);

        private final float renderScale;

        QualityMode(float renderScale) {
            this.renderScale = renderScale;
        }

        public float renderScale() {
            return this.renderScale;
        }

        public String displayName() {
            return this.name().toLowerCase(Locale.ROOT).replace('_', ' ');
        }
    }
}

