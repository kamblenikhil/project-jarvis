#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include "subghz_tx.h"

#define TAG "Jarvis"

#define JARVIS_CONFIG_PATH   "/ext/apps_data/jarvis/config.txt"
#define JARVIS_COUNTER_PATH  "/ext/apps_data/jarvis/counter.bin"
#define JARVIS_MAX_ROOMS   16
#define JARVIS_MAX_CMDS    24
#define JARVIS_NAME_LEN    24

typedef struct {
    char    name[JARVIS_NAME_LEN];
    uint8_t id;
} JarvisEntry;

typedef struct {
    JarvisEntry rooms[JARVIS_MAX_ROOMS];
    JarvisEntry cmds[JARVIS_MAX_ROOMS][JARVIS_MAX_CMDS];
    int         cmd_count[JARVIS_MAX_ROOMS];
    int         room_count;
} JarvisConfig;

typedef struct {
    FuriMessageQueue* event_queue;
    ViewPort*         view_port;
    Gui*              gui;
    bool              running;
    int               selected_room;
    int               selected_cmd;
    int               menu_level;
    uint32_t          counter;
    bool              sending;
    JarvisConfig      config;
} AppState;

// ── Counter persistence ───────────────────────────────

static uint32_t counter_load(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint32_t counter = 0;
    if(storage_file_open(file, JARVIS_COUNTER_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_read(file, &counter, sizeof(counter));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "Counter loaded: %lu", (unsigned long)counter);
    return counter;
}

static void counter_save(uint32_t counter) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, JARVIS_COUNTER_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &counter, sizeof(counter));
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

// ── Config loading ────────────────────────────────────

static bool read_line(File* file, char* buf, size_t max) {
    size_t i = 0;
    uint8_t c;
    while(i < max - 1) {
        if(storage_file_read(file, &c, 1) != 1) break;
        if(c == '\n') break;
        if(c != '\r') buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return i > 0 || !storage_file_eof(file);
}

static void config_load_defaults(JarvisConfig* cfg) {
    cfg->room_count = 1;
    strncpy(cfg->rooms[0].name, "Default", JARVIS_NAME_LEN - 1);
    cfg->rooms[0].id = 0;
    cfg->cmd_count[0] = 2;
    strncpy(cfg->cmds[0][0].name, "Turn On", JARVIS_NAME_LEN - 1);
    cfg->cmds[0][0].id = 0;
    strncpy(cfg->cmds[0][1].name, "Turn Off", JARVIS_NAME_LEN - 1);
    cfg->cmds[0][1].id = 1;
}

static void config_load(JarvisConfig* cfg) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    bool opened = storage_file_open(file, JARVIS_CONFIG_PATH, FSAM_READ, FSOM_OPEN_EXISTING);
    if(!opened) {
        FURI_LOG_W(TAG, "Config not found, using defaults");
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        config_load_defaults(cfg);
        return;
    }

    cfg->room_count = 0;
    int ri = -1;
    char line[64];

    while(read_line(file, line, sizeof(line))) {
        if(line[0] == '\0' || line[0] == '#') continue;

        if(strncmp(line, "room:", 5) == 0) {
            // room:<id>:<name>
            char* rest = line + 5;
            char* colon = strchr(rest, ':');
            if(!colon || colon == rest) continue;
            *colon = '\0';
            int id = atoi(rest);
            char* name = colon + 1;
            if(name[0] == '\0') continue;

            if(cfg->room_count >= JARVIS_MAX_ROOMS) break;
            ri = cfg->room_count++;
            cfg->rooms[ri].id = (uint8_t)id;
            strncpy(cfg->rooms[ri].name, name, JARVIS_NAME_LEN - 1);
            cfg->rooms[ri].name[JARVIS_NAME_LEN - 1] = '\0';
            cfg->cmd_count[ri] = 0;

        } else if(strncmp(line, "cmd:", 4) == 0 && ri >= 0) {
            // cmd:<id>:<name>
            if(cfg->cmd_count[ri] >= JARVIS_MAX_CMDS) continue;
            char* rest = line + 4;
            char* colon = strchr(rest, ':');
            if(!colon || colon == rest) continue;
            *colon = '\0';
            int id = atoi(rest);
            char* name = colon + 1;
            if(name[0] == '\0') continue;

            int ci = cfg->cmd_count[ri]++;
            cfg->cmds[ri][ci].id = (uint8_t)id;
            strncpy(cfg->cmds[ri][ci].name, name, JARVIS_NAME_LEN - 1);
            cfg->cmds[ri][ci].name[JARVIS_NAME_LEN - 1] = '\0';
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    // drop rooms that have no commands
    int out = 0;
    for(int i = 0; i < cfg->room_count; i++) {
        if(cfg->cmd_count[i] > 0) {
            if(out != i) {
                cfg->rooms[out]    = cfg->rooms[i];
                cfg->cmd_count[out] = cfg->cmd_count[i];
                for(int j = 0; j < cfg->cmd_count[i]; j++)
                    cfg->cmds[out][j] = cfg->cmds[i][j];
            }
            out++;
        }
    }
    cfg->room_count = out;

    if(cfg->room_count == 0) {
        FURI_LOG_W(TAG, "Empty/invalid config, using defaults");
        config_load_defaults(cfg);
    }
}

// ── Draw callback ─────────────────────────────────────

static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* state = ctx;
    JarvisConfig* cfg = &state->config;
    canvas_clear(canvas);

    // ── sending screen ────────────────────────────────
    if(state->sending) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Sending...");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, 36, AlignCenter, AlignCenter,
            cfg->rooms[state->selected_room].name);
        canvas_draw_str_aligned(
            canvas, 64, 48, AlignCenter, AlignCenter,
            cfg->cmds[state->selected_room][state->selected_cmd].name);
        return;
    }

    // ── room selection ────────────────────────────────
    if(state->menu_level == 0) {
        int count = cfg->room_count;

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "JARVIS:");
        canvas_draw_line(canvas, 0, 13, 128, 13);

        canvas_set_font(canvas, FontSecondary);

        int scroll = state->selected_room - 3;
        if(scroll < 0) scroll = 0;

        for(int i = 0; i < 4; i++) {
            int idx = scroll + i;
            if(idx >= count) break;

            if(idx == state->selected_room) {
                canvas_draw_box(canvas, 0, 16 + (i * 11), 128, 11);
                canvas_invert_color(canvas);
                canvas_draw_str(canvas, 4, 25 + (i * 11), cfg->rooms[idx].name);
                canvas_invert_color(canvas);
            } else {
                canvas_draw_str(canvas, 4, 25 + (i * 11), cfg->rooms[idx].name);
            }
        }

        if(count > 4) {
            int bar_height = 44 / count;
            if(bar_height < 4) bar_height = 4;
            int bar_y = 16 + (state->selected_room * (44 - bar_height) / (count - 1));
            canvas_draw_box(canvas, 125, 16, 3, 44);
            canvas_invert_color(canvas);
            canvas_draw_box(canvas, 125, bar_y, 3, bar_height);
            canvas_invert_color(canvas);
        }

    // ── command selection ─────────────────────────────
    } else {
        int ri = state->selected_room;
        int count = cfg->cmd_count[ri];

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, cfg->rooms[ri].name);
        canvas_draw_line(canvas, 0, 13, 128, 13);

        canvas_set_font(canvas, FontSecondary);

        int scroll = state->selected_cmd - 3;
        if(scroll < 0) scroll = 0;

        for(int i = 0; i < 4; i++) {
            int idx = scroll + i;
            if(idx >= count) break;

            if(idx == state->selected_cmd) {
                canvas_draw_box(canvas, 0, 16 + (i * 11), 128, 11);
                canvas_invert_color(canvas);
                canvas_draw_str(canvas, 4, 25 + (i * 11), cfg->cmds[ri][idx].name);
                canvas_invert_color(canvas);
            } else {
                canvas_draw_str(canvas, 4, 25 + (i * 11), cfg->cmds[ri][idx].name);
            }
        }

        if(count > 4) {
            int bar_height = 44 / count;
            if(bar_height < 4) bar_height = 4;
            int bar_y = 16 + (state->selected_cmd * (44 - bar_height) / (count - 1));
            canvas_draw_box(canvas, 125, 16, 3, 44);
            canvas_invert_color(canvas);
            canvas_draw_box(canvas, 125, bar_y, 3, bar_height);
            canvas_invert_color(canvas);
        }
    }
}

// ── Input callback ────────────────────────────────────

static void input_callback(InputEvent* event, void* ctx) {
    AppState* state = ctx;
    furi_message_queue_put(state->event_queue, event, FuriWaitForever);
}

// ── Main app entry point ──────────────────────────────

int32_t jarvis_app(void* p) {
    UNUSED(p);

    AppState* state = malloc(sizeof(AppState));
    state->running       = true;
    state->selected_room = 0;
    state->selected_cmd  = 0;
    state->menu_level    = 0;
    state->sending       = false;

    config_load(&state->config);
    state->counter = counter_load();
    FURI_LOG_I(TAG, "Config: %d room(s)", state->config.room_count);

    state->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    state->view_port   = view_port_alloc();
    view_port_draw_callback_set(state->view_port, draw_callback, state);
    view_port_input_callback_set(state->view_port, input_callback, state);
    state->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(state->gui, state->view_port, GuiLayerFullscreen);

    FURI_LOG_I(TAG, "Jarvis started");

    // ── Event loop ────────────────────────────────────
    InputEvent event;
    while(state->running) {
        if(furi_message_queue_get(state->event_queue, &event, 100) == FuriStatusOk) {

            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                int room_count = state->config.room_count;
                int cmd_count  = state->config.cmd_count[state->selected_room];

                if(state->menu_level == 0) {
                    // ── room selection ─────────────────
                    if(event.key == InputKeyUp) {
                        state->selected_room =
                            (state->selected_room - 1 + room_count) % room_count;

                    } else if(event.key == InputKeyDown) {
                        state->selected_room =
                            (state->selected_room + 1) % room_count;

                    } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                        state->menu_level   = 1;
                        state->selected_cmd = 0;

                    } else if(event.key == InputKeyBack && event.type == InputTypeShort) {
                        state->running = false;
                    }

                } else {
                    // ── command selection ──────────────
                    if(event.key == InputKeyUp) {
                        state->selected_cmd =
                            (state->selected_cmd - 1 + cmd_count) % cmd_count;

                    } else if(event.key == InputKeyDown) {
                        state->selected_cmd =
                            (state->selected_cmd + 1) % cmd_count;

                    } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
                        state->sending = true;
                        view_port_update(state->view_port);

                        state->counter++;
                        counter_save(state->counter);

                        uint8_t device_id = state->config.rooms[state->selected_room].id;
                        uint8_t cmd_id =
                            state->config.cmds[state->selected_room][state->selected_cmd].id;

                        bool ok = subghz_tx_send(device_id, cmd_id, state->counter);

                        if(ok) {
                            FURI_LOG_I(
                                TAG,
                                "Sent: device=%d command=%d counter=%lu",
                                device_id,
                                cmd_id,
                                (unsigned long)state->counter);
                        } else {
                            FURI_LOG_E(TAG, "Send failed");
                        }

                        furi_delay_ms(500);

                        state->sending    = false;
                        state->menu_level = 1;

                    } else if(event.key == InputKeyBack && event.type == InputTypeShort) {
                        state->menu_level = 0;
                    }
                }
            }
            view_port_update(state->view_port);
        }
    }

    // ── Cleanup ───────────────────────────────────────
    gui_remove_view_port(state->gui, state->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(state->view_port);
    furi_message_queue_free(state->event_queue);
    free(state);

    return 0;
}
