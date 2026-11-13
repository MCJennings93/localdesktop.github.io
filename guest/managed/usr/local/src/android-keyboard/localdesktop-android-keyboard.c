// An input method for KWin that has the Android keyboard type instead of an on-screen keyboard of
// its own. KWin starts it (--inputmethod, through /usr/local/bin/localdesktop-inputmethod) and
// activates it whenever a text field takes focus, and deactivates it when the field loses focus.
// Each change is written to /tmp/localdesktop-keyboard ("show" or "hide"), which the app watches
// (src/android/external_display.rs) to open or put away the Android keyboard. The keys it types
// reach the text field as ordinary key presses, not through this input method.
//
// Built by guest/build-android-keyboard.sh; protocol: input-method-unstable-v1.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-unstable-v1-client-protocol.h"

static const char *request_path = "/tmp/localdesktop-keyboard";

static struct zwp_input_method_v1 *input_method;
static struct zwp_input_method_context_v1 *active_context;

// Atomic, so the app never reads a half-written request; the rename also updates the mtime it
// watches.
static void request(const char *state) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.%d", request_path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "android-keyboard: %s: %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", state);
    if (fclose(f) != 0 || rename(tmp, request_path) != 0) {
        fprintf(stderr, "android-keyboard: %s: %s\n", request_path, strerror(errno));
        unlink(tmp);
        return;
    }
    fprintf(stderr, "android-keyboard: %s\n", state);
}

static void context_surrounding_text(void *data, struct zwp_input_method_context_v1 *context,
                                     const char *text, uint32_t cursor, uint32_t anchor) {}
static void context_reset(void *data, struct zwp_input_method_context_v1 *context) {}
static void context_content_type(void *data, struct zwp_input_method_context_v1 *context,
                                 uint32_t hint, uint32_t purpose) {}
static void context_invoke_action(void *data, struct zwp_input_method_context_v1 *context,
                                  uint32_t button, uint32_t index) {}
static void context_commit_state(void *data, struct zwp_input_method_context_v1 *context,
                                 uint32_t serial) {}
static void context_preferred_language(void *data, struct zwp_input_method_context_v1 *context,
                                       const char *language) {}

static const struct zwp_input_method_context_v1_listener context_listener = {
    .surrounding_text = context_surrounding_text,
    .reset = context_reset,
    .content_type = context_content_type,
    .invoke_action = context_invoke_action,
    .commit_state = context_commit_state,
    .preferred_language = context_preferred_language,
};

static void im_activate(void *data, struct zwp_input_method_v1 *im,
                        struct zwp_input_method_context_v1 *context) {
    if (active_context) {
        zwp_input_method_context_v1_destroy(active_context);
    }
    active_context = context;
    zwp_input_method_context_v1_add_listener(context, &context_listener, NULL);
    request("show");
}

static void im_deactivate(void *data, struct zwp_input_method_v1 *im,
                          struct zwp_input_method_context_v1 *context) {
    if (context == active_context) {
        active_context = NULL;
        request("hide");
    }
    zwp_input_method_context_v1_destroy(context);
}

static const struct zwp_input_method_v1_listener im_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    if (strcmp(interface, zwp_input_method_v1_interface.name) == 0 && !input_method) {
        input_method = wl_registry_bind(registry, name, &zwp_input_method_v1_interface, 1);
        zwp_input_method_v1_add_listener(input_method, &im_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    // KWin hands over its end of a socket in WAYLAND_SOCKET.
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "android-keyboard: can't connect to the compositor\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!input_method) {
        fprintf(stderr, "android-keyboard: the compositor offers no zwp_input_method_v1\n");
        return 1;
    }
    // A keyboard left up by a previous run goes away until a text field asks again.
    request("hide");
    while (wl_display_dispatch(display) != -1) {
    }
    fprintf(stderr, "android-keyboard: disconnected\n");
    return 0;
}
