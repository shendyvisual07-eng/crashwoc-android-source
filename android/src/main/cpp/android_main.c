#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define TOUCH_POINTS 16

typedef struct TouchPoint {
    SDL_FingerID id;
    float x;
    float y;
    bool active;
} TouchPoint;

typedef struct TouchState {
    bool up, down, left, right;
    bool cross, circle;
    int fingers;
} TouchState;

static void nw_android_touch_update(TouchPoint points[TOUCH_POINTS], const SDL_TouchFingerEvent *event,
                                    bool released)
{
    TouchPoint *slot = NULL;
    for (int i = 0; i < TOUCH_POINTS; i++) {
        if (points[i].active && points[i].id == event->fingerID) {
            slot = &points[i];
            break;
        }
        if (!slot && !points[i].active) {
            slot = &points[i];
        }
    }
    if (!slot) {
        return;
    }
    slot->id = event->fingerID;
    slot->x = event->x;
    slot->y = event->y;
    slot->active = !released;
}

static TouchState nw_android_touch_state(const TouchPoint points[TOUCH_POINTS])
{
    TouchState state = { 0 };
    for (int i = 0; i < TOUCH_POINTS; i++) {
        if (!points[i].active) {
            continue;
        }
        state.fingers++;
        const float x = points[i].x;
        const float y = points[i].y;
        state.left |= x >= 0.04f && x <= 0.13f && y >= 0.70f && y <= 0.82f;
        state.right |= x >= 0.22f && x <= 0.31f && y >= 0.70f && y <= 0.82f;
        state.up |= x >= 0.13f && x <= 0.22f && y >= 0.56f && y <= 0.68f;
        state.down |= x >= 0.13f && x <= 0.22f && y >= 0.84f && y <= 0.96f;
        state.cross |= x >= 0.78f && x <= 0.87f && y >= 0.70f && y <= 0.82f;
        state.circle |= x >= 0.88f && x <= 0.97f && y >= 0.56f && y <= 0.68f;
    }
    return state;
}

static void nw_android_draw_button(SDL_Renderer *renderer, SDL_FRect rect, bool pressed)
{
    if (pressed) {
        SDL_SetRenderDrawColor(renderer, 70, 170, 255, 230);
    } else {
        SDL_SetRenderDrawColor(renderer, 70, 95, 135, 150);
    }
    SDL_RenderFillRect(renderer, &rect);
}

static void nw_android_prepare_data_dirs(void)
{
    char *root = SDL_GetPrefPath("ShendyVisual", "CrashWOC");
    if (!root) {
        SDL_Log("Could not get app-private data path: %s", SDL_GetError());
        return;
    }

    char *imports = NULL;
    char *saves = NULL;
    if (SDL_asprintf(&imports, "%simport", root) >= 0 &&
        SDL_asprintf(&saves, "%ssaves", root) >= 0) {
        if (SDL_CreateDirectory(imports) && SDL_CreateDirectory(saves)) {
            SDL_Log("Android app data ready: %s", root);
        } else {
            SDL_Log("Could not create app data folders: %s", SDL_GetError());
        }
    }
    SDL_free(imports);
    SDL_free(saves);
    SDL_free(root);
}

/*
 * Phase B foundation test: verify app-private storage and touchscreen input
 * while keeping the desktop OpenGL 3.3 renderer isolated from Android.
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    nw_android_prepare_data_dirs();

    SDL_Window *window = SDL_CreateWindow(
        "CrashWOC Android Input Preview",
        1280,
        720,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Log("CrashWOC Android input preview started on %s", SDL_GetPlatform());

    TouchPoint points[TOUCH_POINTS] = { 0 };
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION) {
                nw_android_touch_update(points, &event.tfinger, false);
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                nw_android_touch_update(points, &event.tfinger, true);
            }
        }

        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        TouchState touch = nw_android_touch_state(points);

        SDL_SetRenderDrawColor(renderer, 12, 20, 43, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawColor(renderer, 235, 240, 250, 255);
        SDL_RenderDebugText(renderer, 24.0f, 24.0f, "CRASH WOC - ANDROID INPUT TEST");
        SDL_RenderDebugText(renderer, 24.0f, 48.0f, "Touch the control zones to test multitouch.");
        SDL_RenderDebugTextFormat(renderer, 24.0f, 72.0f, "Active fingers: %d", touch.fingers);
        SDL_RenderDebugTextFormat(renderer, 24.0f, 96.0f, "D-pad U:%d D:%d L:%d R:%d",
                                  touch.up, touch.down, touch.left, touch.right);
        SDL_RenderDebugTextFormat(renderer, 24.0f, 120.0f, "Action Cross:%d Circle:%d",
                                  touch.cross, touch.circle);

        const float w = (float)width;
        const float h = (float)height;
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.04f, h * 0.70f, w * 0.09f, h * 0.12f }, touch.left);
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.22f, h * 0.70f, w * 0.09f, h * 0.12f }, touch.right);
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.13f, h * 0.56f, w * 0.09f, h * 0.12f }, touch.up);
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.13f, h * 0.84f, w * 0.09f, h * 0.12f }, touch.down);
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.78f, h * 0.70f, w * 0.09f, h * 0.12f }, touch.cross);
        nw_android_draw_button(renderer, (SDL_FRect){ w * 0.88f, h * 0.56f, w * 0.09f, h * 0.12f }, touch.circle);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
