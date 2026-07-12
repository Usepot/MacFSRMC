package com.foreground.macfsrmc.client;

import com.foreground.macfsrmc.MacFsrMc;
import net.fabricmc.loader.api.FabricLoader;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;

final class NativeLibraryLoader {
    private static boolean loaded;

    private NativeLibraryLoader() {
    }

    static synchronized void load() throws IOException {
        if (loaded) {
            return;
        }

        String explicitPath = System.getProperty("macfsrmc.nativePath");
        if (explicitPath != null && !explicitPath.isBlank()) {
            System.load(Path.of(explicitPath).toAbsolutePath().normalize().toString());
            loaded = true;
            return;
        }

        Platform platform = Platform.current();
        String resource = "/natives/" + platform.directory + "/" + platform.libraryName;
        byte[] bytes;
        try (InputStream input = NativeLibraryLoader.class.getResourceAsStream(resource)) {
            if (input == null) {
                throw new IOException("Missing bundled native library " + resource);
            }
            bytes = input.readAllBytes();
        }

        String digest = sha256(bytes).substring(0, 16);
        Path directory = FabricLoader.getInstance().getConfigDir()
            .resolve("macfsrmc")
            .resolve("natives")
            .resolve(platform.directory);
        Files.createDirectories(directory);
        Path destination = directory.resolve(digest + "-" + platform.libraryName);
        if (!Files.isRegularFile(destination) || Files.size(destination) != bytes.length) {
            Path temporary = destination.resolveSibling(destination.getFileName() + ".tmp");
            Files.write(temporary, bytes);
            try {
                Files.move(temporary, destination, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (IOException ignored) {
                Files.move(temporary, destination, StandardCopyOption.REPLACE_EXISTING);
            }
        }

        System.load(destination.toAbsolutePath().toString());
        loaded = true;
        MacFsrMc.LOGGER.info("Loaded FSR 2 native bridge for {}", platform.directory);
    }

    private static String sha256(byte[] bytes) throws IOException {
        try {
            return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable", exception);
        }
    }

    private record Platform(String directory, String libraryName) {
        private static Platform current() throws IOException {
            String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
            String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
            boolean arm64 = arch.equals("aarch64") || arch.equals("arm64");
            boolean x64 = arch.equals("amd64") || arch.equals("x86_64");
            if (!arm64 && !x64) {
                throw new IOException("Unsupported CPU architecture: " + arch);
            }

            if (os.contains("win")) {
                if (!x64) {
                    throw new IOException("Windows ARM64 is not currently packaged");
                }
                return new Platform("windows-x86_64", "macfsrmc_fsr2.dll");
            }
            if (os.contains("mac") || os.contains("darwin")) {
                return new Platform(arm64 ? "macos-aarch64" : "macos-x86_64", "libmacfsrmc_fsr2.dylib");
            }
            if (os.contains("linux")) {
                return new Platform(arm64 ? "linux-aarch64" : "linux-x86_64", "libmacfsrmc_fsr2.so");
            }
            throw new IOException("Unsupported operating system: " + os);
        }
    }
}

