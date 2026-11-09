#include "frontend/wizard.h"

#include <float.h>

#include "imgui.h"

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>

static bool nw_wizard_android_disc_picker(const char *target_dir, bool folder)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) {
        SDL_SetError("Android activity is unavailable");
        return false;
    }

    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "openDiscPicker", "(Ljava/lang/String;Z)V") : NULL;
    jstring path = method ? env->NewStringUTF(target_dir) : NULL;
    if (method && path) {
        env->CallVoidMethod(activity, method, path, folder ? JNI_TRUE : JNI_FALSE);
    }
    const bool failed = !method || !path || env->ExceptionCheck();
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (path) env->DeleteLocalRef(path);
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    if (failed) {
        SDL_SetError("Could not open Android Storage Access Framework picker");
        return false;
    }
    return true;
}
#endif

#include "app.h"
#include "frontend/ui.h"

extern "C" {
#include "frontend/settings.h"
#include "frontend/import.h"
#include "platform/paths.h"
#include "platform/input.h"
#include "vfs/extract.h"
}

#define NW_WIZARD_RESCAN_MS 1000

typedef enum NwWizardPage {
    NW_PAGE_PICK,
    NW_PAGE_RUN,
    NW_PAGE_DONE,
    NW_PAGE_READY,
    NW_PAGE_FAILED,
} NwWizardPage;

struct NwWizard {
    SDL_Window *window;
    char *root;
    char *import_dir;
    Uint32 dialog_event; /* file dialog results, pushed from the dialog's thread */
    NwWizardPage page;
    NwImport *job;
    char *image;     /* the image being or last imported */
    bool had_disc;   /* a disc was imported before the wizard opened */
    NwImageList list; /* import folder */
    Uint64 scanned;  /* SDL_GetTicks of the last scan, 0 = never */
    char notice[512];
    char error[512];
    NwSettings settings;
    int section;
    bool settings_dirty;
    int bind_target;
    bool bind_keys;
};

static const SDL_DialogFileFilter nw_wizard_filters[] = {
    { "Disc images (.iso, .cue, .bin)", "iso;cue;bin" },
    { "All files", "*" },
};

static const ImVec4 nw_wizard_ok(0.55f, 0.85f, 0.45f, 1.0f);
static const ImVec4 nw_wizard_warn(1.0f, 0.82f, 0.25f, 1.0f);
static const ImVec4 nw_wizard_bad(1.0f, 0.45f, 0.40f, 1.0f);

static const char *nw_wizard_basename(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

static void nw_wizard_start(NwWizard *w, const char *image)
{
    char *copy = SDL_strdup(image);
    SDL_free(w->image);
    w->image = copy;
    w->notice[0] = '\0';
    w->job = copy ? nw_import_start(copy, w->root, &nw_disc_manifest) : NULL;
    if (w->job) {
        w->page = NW_PAGE_RUN;
    } else {
        SDL_strlcpy(w->error, SDL_GetError(), sizeof(w->error));
        w->page = NW_PAGE_FAILED;
    }
}

/* Runs on the dialog's thread: hands the result to the main thread as a user event. */
static void SDLCALL nw_wizard_picked(void *user, const char *const *files, int filter)
{
    SDL_Event e;
    (void)filter;
    if (files && !files[0]) {
        return; /* cancelled */
    }
    SDL_zero(e);
    e.type = (Uint32)(uintptr_t)user;
    e.user.data1 = files ? SDL_strdup(files[0]) : NULL;
    e.user.data2 = files ? NULL : SDL_strdup(SDL_GetError());
    if (!SDL_PushEvent(&e)) {
        SDL_free(e.user.data1);
        SDL_free(e.user.data2);
    }
}

/* Opens dir in the file manager through a file:// URL. */
static void nw_wizard_open_folder(const char *dir)
{
    size_t n = SDL_strlen(dir);
    char *url = (char *)SDL_malloc(n * 3 + 9), *out = url;
    if (!url) {
        return;
    }
    out += SDL_snprintf(out, 9, "file://%s", dir[0] == '/' ? "" : "/");
    for (const unsigned char *p = (const unsigned char *)dir; *p; p++) {
        if (*p == '\\') {
            *out++ = '/';
        } else if (SDL_isalnum(*p) || SDL_strchr("/:-_.~", *p)) {
            *out++ = (char)*p;
        } else {
            out += SDL_snprintf(out, 4, "%%%02X", *p);
        }
    }
    *out = '\0';
    if (!SDL_OpenURL(url)) {
        SDL_Log("cannot open %s: %s", url, SDL_GetError());
    }
    SDL_free(url);
}

NwWizard *nw_wizard_create(SDL_Window *window, const char *root)
{
    NwWizard *w = (NwWizard *)SDL_calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->window = window;
    w->bind_target = -1;
    w->root = SDL_strdup(root);
    w->import_dir = nw_paths_dir(root, NW_PATHS_IMPORT, false);
    w->dialog_event = SDL_RegisterEvents(1);
    w->had_disc = nw_extract_has_disc(root);
#if defined(__ANDROID__)
    if (w->had_disc) {
        w->page = NW_PAGE_READY;
    }
#endif
    nw_settings_defaults(&w->settings);
    if (!nw_settings_load(&w->settings, root)) {
        SDL_ClearError(); /* a first launch has no settings file yet */
    }
    nw_input_set_controls(&w->settings.controls);
    if (!w->root || !w->import_dir) {
        nw_wizard_destroy(w);
        return NULL;
    }
    return w;
}

void nw_wizard_destroy(NwWizard *w)
{
    if (!w) {
        return;
    }
    nw_import_free(w->job);
    nw_import_list_free(&w->list);
    nw_input_listen(NW_INPUT_LISTEN_OFF);
    SDL_free(w->image);
    SDL_free(w->import_dir);
    SDL_free(w->root);
    SDL_free(w);
}

void nw_wizard_event(NwWizard *w, const SDL_Event *e)
{
    nw_input_event(e);
    if (e->type == SDL_EVENT_DROP_FILE && e->drop.data) {
        if (w->page != NW_PAGE_RUN) {
            nw_wizard_start(w, e->drop.data);
        }
    } else if (w->dialog_event && e->type == w->dialog_event) {
        char *path = (char *)e->user.data1, *why = (char *)e->user.data2;
        if (path && w->page == NW_PAGE_PICK) {
            nw_wizard_start(w, path);
        } else if (why) {
            SDL_snprintf(w->notice, sizeof(w->notice),
                         "No file dialog available (%s). Drop the image here or use the import folder.",
                         why);
        }
        SDL_free(path);
        SDL_free(why);
    }
}

static void nw_wizard_heading(const char *text)
{
    nw_ui_push_font(NW_UI_BOLD, 1.1f);
    ImGui::SeparatorText(text);
    nw_ui_pop_font();
}

static void nw_wizard_title(const char *text)
{
    nw_ui_push_font(NW_UI_BOLD, 1.35f);
    ImGui::TextUnformatted(text);
    nw_ui_pop_font();
}

static void nw_wizard_wrapped(const ImVec4 &color, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/* Starts the next button so that the buttons labelled in labels end flush right. */
static void nw_wizard_align_right(const char *const *labels, int count)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    float width = 0.0f;
    for (int i = 0; i < count; i++) {
        width += ImGui::CalcTextSize(labels[i]).x + style.FramePadding.x * 2.0f + (i ? style.ItemSpacing.x : 0.0f);
    }
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    }
}

static NwWizardResult nw_wizard_pick(NwWizard *w)
{
    NwWizardResult result = NW_WIZARD_OPEN;
    Uint64 now = SDL_GetTicks();
    if (!w->scanned || now - w->scanned >= NW_WIZARD_RESCAN_MS) {
        nw_import_list_free(&w->list);
        if (!nw_import_scan(w->import_dir, &w->list)) {
            SDL_Log("cannot list %s: %s", w->import_dir, SDL_GetError());
        }
        w->scanned = now ? now : 1;
    }

    nw_wizard_title("Welcome");
    ImGui::TextWrapped("NuWorld needs a disc image of your own copy of the game:");
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("Crash Bandicoot: The Wrath of Cortex (PS2, PAL v1.03, SLES-50386)");
    ImGui::BulletText(".iso, or .bin with its .cue");

    if (w->had_disc) {
        nw_wizard_wrapped(nw_wizard_ok, "A disc is already imported. It is only replaced if the new import works.");
    }
    if (w->notice[0]) {
        nw_wizard_wrapped(nw_wizard_warn, w->notice);
    }

    nw_wizard_heading("Disc image");
#if defined(__ANDROID__)
    if (ImGui::Button("Choose ISO file")) {
        if (!nw_wizard_android_disc_picker(w->import_dir, false)) {
            SDL_strlcpy(w->error, SDL_GetError(), sizeof(w->error));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import BIN/CUE folder")) {
        if (!nw_wizard_android_disc_picker(w->import_dir, true)) {
            SDL_strlcpy(w->error, SDL_GetError(), sizeof(w->error));
        }
    }
    ImGui::TextDisabled("Android copies selected files into the private import folder.");
#else
    ImGui::BeginDisabled(!w->dialog_event);
    if (ImGui::Button("Choose file...")) {
        SDL_ShowOpenFileDialog(nw_wizard_picked, (void *)(uintptr_t)w->dialog_event, w->window, nw_wizard_filters,
                               (int)SDL_arraysize(nw_wizard_filters), NULL, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("or drop it here");
#endif

    nw_wizard_heading("Import folder");
    ImGui::TextWrapped("Or copy it into this folder and pick it below (Steam Deck: use Desktop Mode).");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", w->import_dir);
    ImGui::PopStyleColor();
    if (ImGui::Button("Open folder")) {
        nw_wizard_open_folder(w->import_dir);
    }

    const ImGuiTableFlags table = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("images", 2, table)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed);
        for (size_t i = 0; i < w->list.count; i++) {
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", w->list.names[i]);
            ImGui::TableNextColumn();
            if (ImGui::Button("Import")) {
                char *path = NULL;
                if (SDL_asprintf(&path, "%s%s", w->import_dir, w->list.names[i]) >= 0) {
                    nw_wizard_start(w, path);
                    SDL_free(path);
                }
            }
            ImGui::PopID();
        }
        if (!w->list.count) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("No disc images found.");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Data folder: %s", w->root);
    ImGui::PopStyleColor();
    static const char *const both[] = { "Close", "Quit" }, *const quit[] = { "Quit" };
    nw_wizard_align_right(w->had_disc ? both : quit, w->had_disc ? 2 : 1);
    if (w->had_disc) {
        if (ImGui::Button("Close")) {
            result = NW_WIZARD_CLOSED;
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Quit")) {
        result = NW_WIZARD_QUIT;
    }
    return result;
}

static void nw_wizard_run(NwWizard *w)
{
    uint64_t done, total;
    NwImportState state = nw_import_poll(w->job, &done, &total);
    if (state != NW_IMPORT_RUNNING) {
        if (state == NW_IMPORT_FAILED) {
            SDL_strlcpy(w->error, nw_import_error(w->job), sizeof(w->error));
        }
        nw_import_free(w->job);
        w->job = NULL;
        w->page = state == NW_IMPORT_DONE ? NW_PAGE_DONE : state == NW_IMPORT_FAILED ? NW_PAGE_FAILED : NW_PAGE_PICK;
        if (state == NW_IMPORT_CANCELLED) {
            SDL_strlcpy(w->notice, "Import cancelled. Nothing was changed.", sizeof(w->notice));
        }
        return;
    }

    nw_wizard_title("Importing");
    ImGui::TextWrapped("%s", nw_wizard_basename(w->image));
    ImGui::Spacing();
    if (!total) {
        ImGui::TextUnformatted("Checking the disc...");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0.0f), "");
    } else {
        char overlay[32];
        float fraction = (float)((double)done / (double)total);
        SDL_snprintf(overlay, sizeof(overlay), "%d%%", (int)(done * 100 / total));
        ImGui::TextUnformatted("Copying and checking files...");
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), overlay);
        ImGui::TextDisabled("%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
    }
    ImGui::Spacing();
    static const char *const cancel[] = { "Cancel" };
    nw_wizard_align_right(cancel, 1);
    if (ImGui::Button("Cancel")) {
        nw_import_cancel(w->job);
    }
}

static NwWizardResult nw_wizard_done(NwWizard *w)
{
    nw_wizard_title("Disc imported");
    nw_wizard_wrapped(nw_wizard_ok, "Every file matches PAL v" NW_DISC_VER);
    ImGui::TextWrapped("The game files are in %s" NW_DISC_DIR ". You will not need the disc image again, you can delete it if you want!",
                       w->root);
    ImGui::Spacing();
    static const char *const next[] = { "Continue" };
    nw_wizard_align_right(next, 1);
    if (ImGui::Button("Continue")) {
#if defined(NW_WITH_GAME)
        return NW_WIZARD_CLOSED;
#else
        w->page = NW_PAGE_READY;
#endif
    }
    return NW_WIZARD_OPEN;
}

static NwWizardResult nw_wizard_ready(NwWizard *w)
{
    NwWizardResult result = NW_WIZARD_OPEN;
    nw_wizard_title("Disc ready");
    nw_wizard_wrapped(nw_wizard_ok, "Your PAL v" NW_DISC_VER " game data has been imported and verified.");
    ImGui::Spacing();
    ImGui::TextWrapped("Your imported game data is safe. You do not need to import the disc again.");
    ImGui::Spacing();
#if defined(__ANDROID__) && defined(NW_WITH_GAME)
    if (ImGui::Button("START GAME", ImVec2(-FLT_MIN, 54.0f))) {
        result = NW_WIZARD_START_GAME;
    }
    ImGui::Spacing();
#elif defined(__ANDROID__)
    ImGui::TextWrapped("This build does not include the game runtime.");
    ImGui::Spacing();
#endif
    if (ImGui::Button("Back to Control Center")) {
        w->section = 1;
    }
    return result;
}

static void nw_wizard_failed(NwWizard *w)
{
    nw_wizard_title("Import failed");
    ImGui::TextWrapped("%s", nw_wizard_basename(w->image ? w->image : ""));
    nw_wizard_wrapped(nw_wizard_bad, w->error);
    ImGui::TextWrapped("NuWorld runs only the PAL version 1.03 (SLES-50386). The data folder is unchanged.");
    ImGui::Spacing();
    static const char *const buttons[] = { "Try again", "Back" };
    nw_wizard_align_right(buttons, 2);
    if (ImGui::Button("Try again") && w->image) {
        nw_wizard_start(w, w->image); /* copies the path before freeing the old one */
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) {
        w->page = NW_PAGE_PICK;
    }
}

static void nw_wizard_graphics(NwWizard *w)
{
    static const char *const vsyncs[] = { "Off", "On", "Adaptive" };
    static const char *const filters[] = { "Retail", "Nearest", "Bilinear" };
    static const char *const scales[] = { "Auto", "1x (PS2)", "2x", "3x", "4x", "5x", "6x", "7x", "8x" };
    static const int samples[] = { 0, 2, 4, 8 };
    static const char *const sample_names[] = { "Off", "2x", "4x", "8x" };
    NwSettings *s = &w->settings;
    int fps = s->fps == 0 ? 0 : s->fps == 30 ? 1 : s->fps == 60 ? 2 : s->fps == 90 ? 3 : s->fps == 120 ? 4 : 5;
    const char *const fps_names[] = { "Display refresh", "30 FPS", "60 FPS", "90 FPS", "120 FPS", "1000 FPS (max)" };
    if (ImGui::BeginTable("graphics", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("FPS limit");
        ImGui::TableNextColumn();
        if (ImGui::Combo("##fps", &fps, fps_names, (int)SDL_arraysize(fps_names))) {
            s->fps = fps == 0 ? 0 : fps == 5 ? 1000 : fps == 1 ? 30 : fps == 2 ? 60 : fps == 3 ? 90 : 120;
            w->settings_dirty = true;
        }
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("VSync");
        ImGui::TableNextColumn();
        int vsync = (int)s->vsync;
        if (ImGui::Combo("##vsync", &vsync, vsyncs, NW_VSYNC_MODES)) { s->vsync = (NwVsync)vsync; w->settings_dirty = true; }
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("Internal resolution");
        ImGui::TableNextColumn();
        if (ImGui::Combo("##scale", &s->scale, scales, (int)SDL_arraysize(scales))) w->settings_dirty = true;
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("MSAA");
        ImGui::TableNextColumn();
        int msaa = 0; while (msaa < 3 && samples[msaa + 1] <= s->msaa) msaa++;
        if (ImGui::Combo("##msaa", &msaa, sample_names, 4)) { s->msaa = samples[msaa]; w->settings_dirty = true; }
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("Texture filter");
        ImGui::TableNextColumn();
        int filter = (int)s->filter;
        if (ImGui::Combo("##filter", &filter, filters, NW_FILTERS)) { s->filter = (NwTexFilter)filter; w->settings_dirty = true; }
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted("Frame interpolation");
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("##interp", &s->interpolation)) w->settings_dirty = true;
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Some graphics options apply when the game renderer starts.");
}

static void nw_wizard_controller(NwWizard *w)
{
    NwControls *c = &w->settings.controls;
    static const char *const prompt_names[] = { "Auto", "Xbox", "PlayStation", "Nintendo", "Steam Deck", "Keyboard" };

    /* Complete a pending capture on the frame after SDL delivered the input event. */
    if (w->bind_target >= 0) {
        int heard = nw_input_heard();
        if (heard >= 0) {
            if (heard > 0) {
                uint16_t(*binds)[NW_CONTROL_BINDS] = w->bind_keys ? c->keys : c->pad;
                int count = w->bind_keys ? NW_CONTROL_TARGETS : NW_CONTROL_BUTTONS;
                nw_controls_bind(binds, count, w->bind_target, (uint16_t)heard);
                w->settings_dirty = true;
                nw_input_set_controls(c);
            }
            w->bind_target = -1;
            nw_input_listen(NW_INPUT_LISTEN_OFF);
        }
    }

    ImGui::TextUnformatted("TOUCH CONTROLS");
    ImGui::TextWrapped("The existing on-screen touch controls remain enabled. Their layout is not changed here.");
    ImGui::SeparatorText("GAMEPAD & KEYBOARD MAPPING");
    const char *pad_name = nw_input_pad_name();
    ImGui::Text("Connected gamepad: %s", pad_name ? pad_name : "Not detected - connect a controller");
    ImGui::TextDisabled("Tap a mapping field, then press the button or key you want to assign.");
    ImGui::Spacing();

    if (ImGui::BeginTable("controller_mapping", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY, ImVec2(0, 310.0f))) {
        ImGui::TableSetupColumn("GAME ACTION", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("GAMEPAD", ImGuiTableColumnFlags_WidthStretch, 0.31f);
        ImGui::TableSetupColumn("KEYBOARD", ImGuiTableColumnFlags_WidthStretch, 0.31f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NW_CONTROL_TARGETS; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", nw_control_targets[i].label);
            if (i < NW_CONTROL_BUTTONS) {
                ImGui::TableNextColumn();
                const char *pad_label = c->pad[i][0] ? nw_input_pad_label(c->pad[i][0]) : NULL;
                char pad_button[96];
                SDL_snprintf(pad_button, sizeof(pad_button), "%s##pad", w->bind_target == i && !w->bind_keys
                    ? "Press a button..." : (pad_label ? pad_label : "Unassigned"));
                if (ImGui::Button(pad_button, ImVec2(-FLT_MIN, 0))) {
                    w->bind_target = i;
                    w->bind_keys = false;
                    nw_input_listen(NW_INPUT_LISTEN_PAD);
                }
                if (w->bind_target == i && !w->bind_keys) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Cancel##pad")) {
                        w->bind_target = -1;
                        nw_input_listen(NW_INPUT_LISTEN_OFF);
                    }
                }
            } else {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Analog stick");
            }
            ImGui::TableNextColumn();
            const char *key_label = c->keys[i][0] ? nw_controls_key_name(c->keys[i][0]) : NULL;
            char key_button[96];
            SDL_snprintf(key_button, sizeof(key_button), "%s##key", w->bind_target == i && w->bind_keys
                ? "Press a key..." : (key_label ? key_label : "Unassigned"));
            if (ImGui::Button(key_button, ImVec2(-FLT_MIN, 0))) {
                w->bind_target = i;
                w->bind_keys = true;
                nw_input_listen(NW_INPUT_LISTEN_KEYS);
            }
            if (w->bind_target == i && w->bind_keys) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##key")) {
                    w->bind_target = -1;
                    nw_input_listen(NW_INPUT_LISTEN_OFF);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (w->bind_target >= 0) {
        ImGui::TextColored(nw_wizard_warn, "Listening: press a %s for %s. Escape cancels.",
            w->bind_keys ? "keyboard key" : "gamepad button/trigger", nw_control_targets[w->bind_target].label);
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset mappings")) {
        nw_controls_defaults(c);
        w->bind_target = -1;
        nw_input_listen(NW_INPUT_LISTEN_OFF);
        nw_input_set_controls(c);
        w->settings_dirty = true;
    }
    ImGui::SameLine();
    int prompts = (int)c->prompts;
    if (ImGui::Combo("Button prompts", &prompts, prompt_names, NW_PROMPT_STYLES)) {
        c->prompts = (NwPromptStyle)prompts;
        w->settings_dirty = true;
        nw_input_set_controls(c);
    }
    if (ImGui::SliderInt("Left stick deadzone", &c->left_deadzone, 0, 90, "%d%%")) {
        w->settings_dirty = true; nw_input_set_controls(c);
    }
    if (ImGui::SliderInt("Right stick deadzone", &c->right_deadzone, 0, 90, "%d%%")) {
        w->settings_dirty = true; nw_input_set_controls(c);
    }
    if (ImGui::SliderInt("Trigger threshold", &c->trigger_press, 1, 100, "%d%%")) {
        w->settings_dirty = true; nw_input_set_controls(c);
    }
    if (ImGui::SliderInt("Rumble strength", &c->rumble, 0, 100, "%d%%")) {
        w->settings_dirty = true; nw_input_set_controls(c);
    }
}

static void nw_wizard_settings(NwWizard *w)
{
    if (w->section == 1) nw_wizard_graphics(w);
    else if (w->section == 2) nw_wizard_controller(w);
    else {
        ImGui::TextUnformatted("SYSTEM");
        ImGui::Separator();
        ImGui::TextWrapped("CrashWOC NuWorld Android preview");
        ImGui::TextWrapped("Settings are stored in this app's private data folder.");
        ImGui::TextWrapped("The game requires your own PAL v1.03 disc image (SLES-50386).");
    }
    ImGui::Spacing();
    if (w->settings_dirty) {
        ImGui::TextColored(nw_wizard_warn, "You have unsaved changes.");
    } else {
        ImGui::TextDisabled("All settings are saved.");
    }
    if (ImGui::Button("Save settings")) {
        if (nw_settings_save(&w->settings, w->root)) {
            w->settings_dirty = false;
            nw_input_set_controls(&w->settings.controls);
            SDL_strlcpy(w->notice, "Settings saved.", sizeof(w->notice));
        } else {
            SDL_strlcpy(w->error, SDL_GetError(), sizeof(w->error));
        }
    }
    if (w->notice[0]) ImGui::SameLine(); 
    if (w->notice[0]) ImGui::TextColored(nw_wizard_ok, "%s", w->notice);
    if (w->error[0]) ImGui::TextColored(nw_wizard_bad, "%s", w->error);
}

NwWizardResult nw_wizard_draw(NwWizard *w)
{
    NwWizardResult result = NW_WIZARD_OPEN;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const ImVec2 screen_min = vp->Pos;
    const ImVec2 screen_max(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

    /* Soft tropical dusk backdrop, drawn without external textures. */
    bg->AddRectFilledMultiColor(screen_min, screen_max,
        IM_COL32(8, 18, 43, 255), IM_COL32(20, 45, 79, 255),
        IM_COL32(6, 15, 34, 255), IM_COL32(10, 27, 53, 255));
    const float unit = SDL_min(vp->Size.x, vp->Size.y);
    bg->AddCircleFilled(ImVec2(screen_max.x - unit * 0.15f, screen_min.y + unit * 0.17f),
                        unit * 0.12f, IM_COL32(255, 160, 72, 24), 48);
    bg->AddCircleFilled(ImVec2(screen_min.x + unit * 0.08f, screen_max.y - unit * 0.08f),
                        unit * 0.22f, IM_COL32(31, 179, 154, 20), 48);
    bg->AddCircleFilled(ImVec2(screen_max.x - unit * 0.04f, screen_max.y + unit * 0.08f),
                        unit * 0.25f, IM_COL32(24, 105, 116, 28), 48);

    /* Center the whole interface and keep a comfortable safe margin on short screens. */
    const float margin_x = SDL_max(10.0f, vp->Size.x * 0.025f);
    const float margin_y = SDL_max(8.0f, vp->Size.y * 0.035f);
    /* Use nearly the full landscape viewport. A desktop-style fixed max width
       made the Android panel look like a narrow floating dialog on tablets/phones. */
    const float width = SDL_max(1.0f, vp->Size.x - margin_x * 2.0f);
    const float height = SDL_max(1.0f, vp->Size.y - margin_y * 2.0f);
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - width) * 0.5f,
                     vp->Pos.y + (vp->Size.y - height) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 15.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.085f, 0.15f, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.16f, 0.25f, 0.62f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.48f, 0.70f, 0.88f, 0.24f));

    ImGui::Begin("CRASH WOC CONTROL CENTER", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList *panel = ImGui::GetWindowDrawList();
    const ImVec2 panel_min = ImGui::GetWindowPos();
    const ImVec2 panel_max(panel_min.x + ImGui::GetWindowSize().x, panel_min.y + ImGui::GetWindowSize().y);
    panel->AddRect(panel_min, panel_max, IM_COL32(124, 190, 231, 90), 20.0f, 1.2f, ImDrawFlags_RoundCornersAll);
    ImGui::TextColored(ImVec4(1.0f, 0.57f, 0.20f, 1.0f), "CRASH WOC");
    ImGui::SameLine();
    ImGui::TextDisabled("NUWORLD  /  ANDROID CONTROL CENTER");
    ImGui::Separator();

    const float available = ImGui::GetContentRegionAvail().x;
    const float nav_width = SDL_max(150.0f, available * 0.225f);
    ImGui::BeginChild("navigation", ImVec2(nav_width, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("MENU");
    ImGui::Spacing();
    const float nav_button_h = SDL_max(34.0f, ImGui::GetTextLineHeightWithSpacing() * 1.65f);
    if (ImGui::Selectable("  DISC IMPORT", w->section == 0, 0, ImVec2(0, nav_button_h))) w->section = 0;
    if (ImGui::Selectable("  GRAPHICS", w->section == 1, 0, ImVec2(0, nav_button_h))) w->section = 1;
    if (ImGui::Selectable("  CONTROLLER", w->section == 2, 0, ImVec2(0, nav_button_h))) w->section = 2;
    if (ImGui::Selectable("  SYSTEM", w->section == 3, 0, ImVec2(0, nav_button_h))) w->section = 3;
    const float bottom_space = ImGui::GetContentRegionAvail().y;
    if (bottom_space > 48.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + bottom_space - 42.0f);
    if (ImGui::Button("EXIT APP", ImVec2(-FLT_MIN, 40.0f))) result = NW_WIZARD_QUIT;
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled(w->section == 0 ? "FIRST-TIME SETUP" : "SETTINGS");
    if (w->section == 1) ImGui::TextUnformatted("GRAPHICS & PERFORMANCE");
    else if (w->section == 2) ImGui::TextUnformatted("CONTROLLER");
    else if (w->section == 3) ImGui::TextUnformatted("SYSTEM INFORMATION");
    else ImGui::TextUnformatted("GAME DISC");
    ImGui::Separator();
    if (w->section == 0) {
        switch (w->page) {
        case NW_PAGE_PICK: result = nw_wizard_pick(w); break;
        case NW_PAGE_RUN: nw_wizard_run(w); break;
        case NW_PAGE_DONE: result = nw_wizard_done(w); break;
        case NW_PAGE_READY: result = nw_wizard_ready(w); break;
        case NW_PAGE_FAILED: nw_wizard_failed(w); break;
        }
    } else {
        nw_wizard_settings(w);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    return result;
}
