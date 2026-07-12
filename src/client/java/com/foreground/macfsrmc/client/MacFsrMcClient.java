package com.foreground.macfsrmc.client;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientLifecycleEvents;

public final class MacFsrMcClient implements ClientModInitializer {
    private static Fsr2Config config;
    private static Fsr2Controller controller;

    @Override
    public void onInitializeClient() {
        controller();
        ClientLifecycleEvents.CLIENT_STOPPING.register(client -> closeController());
    }

    public static synchronized Fsr2Controller controller() {
        if (controller == null) {
            config = Fsr2Config.load();
            controller = new Fsr2Controller(config);
        }
        return controller;
    }

    public static synchronized void closeController() {
        if (controller != null) {
            controller.close();
            controller = null;
        }
    }
}
