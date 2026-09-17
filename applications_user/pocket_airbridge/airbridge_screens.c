#include "airbridge_screens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <furi.h>

#define TAG "AirBridge"

struct AirbridgeScreens {
    AirbridgeScreen current;
    AirbridgeError error;
    AirbridgeScreenActions actions;
    void* context;
    AirbridgeTypingTransport prompt_transport;
    AirbridgeTypingTransport armed_transport;
    bool deploy_request_accepted;
    uint32_t waiting_pump_tick;
};

static void airbridge_screens_set_error(
    AirbridgeError* error,
    const char* title,
    const char* detail,
    const char* action) {
    snprintf(error->title, sizeof(error->title), "%s", title);
    snprintf(error->detail, sizeof(error->detail), "%s", detail);
    snprintf(error->action, sizeof(error->action), "%s", action);
}

static void airbridge_screens_describe_error(AirbridgeError* error, const char* message) {
    const char* title = "Deploy error";
    const char* detail = message;
    const char* action = "Retry after BACK";
    if(strstr(message, "bootstrap") != NULL || strstr(message, "BYTE 0x") != NULL) {
        title = strstr(message, "NO ") == message ? "Bootstrap missing" : "Invalid bootstrap";
        action = "Fix asset, then BACK";
    } else if(strstr(message, "app-usb") != NULL || strstr(message, "app-ble") != NULL) {
        title = "App bundle unavailable";
        action = "Copy bundle, then BACK";
    } else if(strcmp(message, "KEYBOARD SEND ERROR") == 0) {
        title = "Keyboard link lost";
        detail = "Could not send a key";
        action = "Check host; tap a key";
    } else if(strcmp(message, "BLE LINK TIMEOUT") == 0) {
        title = "BLE link timed out";
        detail = "Keyboard host disconnected";
        action = "Reconnect, then BACK";
    } else if(strstr(message, "STREAM") != NULL) {
        title = "Transfer failed";
        detail = strcmp(message, "STREAM STALLED") == 0 ? "BLE stream stalled" :
                                                          "Bundle stream stopped";
    } else if(strcmp(message, "DEPLOY NOT ARMED") == 0) {
        title = "Deploy rejected";
        detail = "Request outside Waiting";
        action = "BACK, then arm deploy";
    } else if(strcmp(message, "STUCK KEY - TAP A KEY") == 0) {
        title = "Key release failed";
        detail = "Key may remain pressed";
        action = "Tap a key, then BACK";
    } else if(strcmp(message, "Set USB to Kbd+Vendor") == 0) {
        title = "USB profile cannot type";
        detail = "Select Kbd+Vendor profile";
    }
    airbridge_screens_set_error(error, title, detail, action);
}

AirbridgeScreens* airbridge_screens_alloc(const AirbridgeScreenActions* actions, void* context) {
    AirbridgeScreens* screens = malloc(sizeof(*screens));
    if(screens == NULL) return NULL;
    memset(screens, 0, sizeof(*screens));
    screens->current = AirbridgeScreenBridge;
    screens->actions = *actions;
    screens->context = context;
    return screens;
}

void airbridge_screens_free(AirbridgeScreens* screens) {
    free(screens);
}

AirbridgeScreen airbridge_screens_current(const AirbridgeScreens* screens) {
    return screens->current;
}

const AirbridgeError* airbridge_screens_error(const AirbridgeScreens* screens) {
    return &screens->error;
}

/* Deploy slots retain their own transport; utility screens never arm a stream. */
static int airbridge_screens_slot_of(const AirbridgeScreens* screens) {
    if(screens->current == AirbridgeScreenSettings) return 3;
    if(screens->current == AirbridgeScreenPasswords) return 4;
    if(screens->current != AirbridgeScreenDeployPrompt) return 0;
    return screens->prompt_transport == AirbridgeTypingTransportBle ? 2 : 1;
}

static void airbridge_screens_carousel_next(AirbridgeScreens* screens, int direction) {
    const int next = (airbridge_screens_slot_of(screens) + direction + 5) % 5;
    if(next == 1 &&
       !screens->actions.deploy_supported(screens->context, AirbridgeTypingTransportUsb)) {
        airbridge_screens_show_error(screens, "Set USB to Kbd+Vendor");
        return;
    }
    if(next == 2 &&
       !screens->actions.deploy_supported(screens->context, AirbridgeTypingTransportBle)) {
        airbridge_screens_show_error(screens, "BLE NOT INSTALLED");
        return;
    }
    if(next == 0) {
        screens->current = AirbridgeScreenBridge;
    } else if(next >= 3) {
        screens->current = next == 3 ? AirbridgeScreenSettings : AirbridgeScreenPasswords;
    } else {
        screens->current = AirbridgeScreenDeployPrompt;
        screens->prompt_transport = next == 2 ? AirbridgeTypingTransportBle :
                                                AirbridgeTypingTransportUsb;
    }
    if(screens->actions.menu_enter)
        screens->actions.menu_enter(screens->context, screens->current);
}

void airbridge_screens_show_error(void* context, const char* message) {
    AirbridgeScreens* screens = context;
    if(screens->current == AirbridgeScreenError) {
        if(strcmp(message, "STUCK KEY - TAP A KEY") == 0) {
            airbridge_screens_describe_error(&screens->error, message);
        } else {
            FURI_LOG_W(TAG, "Preserving first error; ignored: %s", message);
        }
        return;
    }
    if(screens->current == AirbridgeScreenTyping ||
       screens->current == AirbridgeScreenPasswordTyping) {
        if(!screens->actions.typing_abort(screens->context)) {
            airbridge_screens_describe_error(&screens->error, "STUCK KEY - TAP A KEY");
            screens->current = AirbridgeScreenError;
            return;
        }
    } else if(screens->current == AirbridgeScreenStreaming) {
        screens->actions.stream_close(screens->context);
    }
    airbridge_screens_describe_error(&screens->error, message);
    screens->current = AirbridgeScreenError;
}

void airbridge_screens_show_fatal(AirbridgeScreens* screens, const char* message) {
    airbridge_screens_set_error(
        &screens->error, "Fatal startup error", message, "Long BACK: exit");
    screens->current = AirbridgeScreenFatal;
}

void airbridge_screens_typing_complete(AirbridgeScreens* screens) {
    if(screens->current == AirbridgeScreenPasswordTyping) {
        screens->current = AirbridgeScreenPasswords;
        return;
    }
    if(screens->current == AirbridgeScreenTyping) {
        screens->current = AirbridgeScreenWaiting;
        screens->waiting_pump_tick = furi_get_tick();
    }
}

void airbridge_screens_deploy_requested(
    AirbridgeScreens* screens,
    AirbridgeTypingTransport transport) {
    if(screens->current == AirbridgeScreenWaiting) {
        /* The relay only forwards direction-matching requests, so a mismatch
         * here is stale input: hold Waiting rather than erroring out. */
        if(transport != screens->armed_transport) return;
        if(screens->actions.stream_start(screens->context, transport)) {
            screens->current = AirbridgeScreenStreaming;
            screens->deploy_request_accepted = true;
        }
    } else if(screens->current != AirbridgeScreenBridge) {
        airbridge_screens_show_error(screens, "DEPLOY NOT ARMED");
    }
}

AirbridgeTypingTransport airbridge_screens_armed_transport(const AirbridgeScreens* screens) {
    return screens->armed_transport;
}

AirbridgeTypingTransport airbridge_screens_render_transport(const AirbridgeScreens* screens) {
    if(screens->current == AirbridgeScreenDeployPrompt) return screens->prompt_transport;
    return screens->armed_transport;
}

bool airbridge_screens_deploy_request_accepted(const AirbridgeScreens* screens) {
    return screens->deploy_request_accepted;
}

void airbridge_screens_note_ble_reconnect(AirbridgeScreens* screens) {
    screens->deploy_request_accepted = false;
}

uint32_t airbridge_screens_waiting_pump_tick(const AirbridgeScreens* screens) {
    return screens->waiting_pump_tick;
}

void airbridge_screens_waiting_pump_mark(AirbridgeScreens* screens, uint32_t tick) {
    screens->waiting_pump_tick = tick;
}

void airbridge_screens_stream_complete(AirbridgeScreens* screens) {
    if(screens->current == AirbridgeScreenStreaming) {
        screens->current = AirbridgeScreenDone;
    }
}

bool airbridge_screens_handle_ui_intent(void* context, AirbridgeUiIntent intent) {
    AirbridgeScreens* screens = context;
    if(intent == AirbridgeUiIntentExit) {
        screens->actions.exit(screens->context);
        return true;
    }
    if(intent == AirbridgeUiIntentInputDropped) {
        screens->actions.input_dropped(screens->context);
        return false;
    }
    if(screens->current == AirbridgeScreenFatal) return false;

    if(screens->current == AirbridgeScreenSettings ||
       screens->current == AirbridgeScreenPasswords) {
        if(intent == AirbridgeUiIntentBack) {
            screens->current = AirbridgeScreenBridge;
            screens->actions.menu_enter(screens->context, screens->current);
        } else if(intent == AirbridgeUiIntentPrevious || intent == AirbridgeUiIntentNext) {
            airbridge_screens_carousel_next(screens, intent == AirbridgeUiIntentPrevious ? -1 : 1);
        } else if(intent == AirbridgeUiIntentUp || intent == AirbridgeUiIntentResetBle) {
            if(screens->current == AirbridgeScreenPasswords)
                screens->actions.menu_move(
                    screens->context, intent == AirbridgeUiIntentUp ? -1 : 1);
        } else if(intent == AirbridgeUiIntentConfirm) {
            if(screens->actions.menu_confirm(screens->context, screens->current)) {
                screens->armed_transport = AirbridgeTypingTransportUsb;
                screens->current = AirbridgeScreenPasswordTyping;
            }
        }
        return false;
    }
    if(screens->current == AirbridgeScreenPasswordTyping) {
        if(intent == AirbridgeUiIntentBack) {
            if(screens->actions.typing_abort(screens->context)) {
                screens->current = AirbridgeScreenPasswords;
            } else {
                airbridge_screens_show_error(screens, "STUCK KEY - TAP A KEY");
            }
        }
        return false;
    }
    if(screens->current == AirbridgeScreenBridge) {
        if(intent == AirbridgeUiIntentPrevious) {
            airbridge_screens_carousel_next(screens, -1);
        } else if(intent == AirbridgeUiIntentNext) {
            airbridge_screens_carousel_next(screens, 1);
        } else if(
            intent == AirbridgeUiIntentResetBle &&
            screens->actions.ble_profile_installed(screens->context)) {
            FURI_LOG_W(TAG, "Manual BLE reset");
            screens->actions.ble_reset(screens->context);
        }
        return false;
    }
    if(screens->current == AirbridgeScreenDeployPrompt) {
        if(intent == AirbridgeUiIntentBack) {
            screens->current = AirbridgeScreenBridge;
        } else if(intent == AirbridgeUiIntentConfirm) {
            if(screens->actions.typing_start(screens->context, screens->prompt_transport)) {
                screens->armed_transport = screens->prompt_transport;
                screens->current = AirbridgeScreenTyping;
            }
        } else if(intent == AirbridgeUiIntentPrevious) {
            airbridge_screens_carousel_next(screens, -1);
        } else if(intent == AirbridgeUiIntentNext) {
            airbridge_screens_carousel_next(screens, 1);
        }
        return false;
    }
    if(screens->current == AirbridgeScreenTyping && intent == AirbridgeUiIntentBack) {
        if(screens->actions.typing_abort(screens->context)) {
            screens->current = AirbridgeScreenBridge;
        } else {
            airbridge_screens_show_error(screens, "STUCK KEY - TAP A KEY");
        }
        return false;
    }
    if(screens->current == AirbridgeScreenStreaming && intent == AirbridgeUiIntentBack) {
        screens->actions.stream_close(screens->context);
        screens->current = AirbridgeScreenBridge;
        return false;
    }
    if(screens->current == AirbridgeScreenWaiting && intent == AirbridgeUiIntentBack) {
        screens->current = AirbridgeScreenBridge;
        return false;
    }
    if(intent == AirbridgeUiIntentBack || intent == AirbridgeUiIntentConfirm) {
        screens->current = AirbridgeScreenBridge;
    }
    return false;
}
