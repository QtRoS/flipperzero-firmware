#include <furi.h>
#include <furi_hal.h>

#include <hex_viewer_icons.h>
#include <gui/gui.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#include <lib/flipper_format/flipper_format.h>

#include <toolbox/stream/file_stream.h>

#define TAG "HexViewer"

#define HEX_VIEWER_APP_PATH_FOLDER "/any"
#define HEX_VIEWER_APP_EXTENSION "*"

#define HEX_VIEWER_BYTES_PER_ROW 4
#define HEX_VIEWER_ROW_COUNT 4

typedef struct {
    uint8_t file_bytes[HEX_VIEWER_ROW_COUNT][HEX_VIEWER_ROW_COUNT];
    uint32_t line;
    uint32_t read_bytes;
    bool mode; // Print address or content
} HexViewerModel;

typedef struct {
    HexViewerModel* model;
    FuriMutex** model_mutex;

    FuriMessageQueue* input_queue;

    ViewPort* view_port;
    Gui* gui;
} HexViewer;

static void render_callback(Canvas* canvas, void* ctx) {
    HexViewer* hex_viewer = ctx;
    furi_check(furi_mutex_acquire(hex_viewer->model_mutex, FuriWaitForever) == FuriStatusOk);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 12, "HexViewer");

    char temp_buf[32];
    int ROW_HEIGHT = 12;
    int TOP_OFFSET = 24;

    uint32_t row_iters = hex_viewer->model->read_bytes / HEX_VIEWER_BYTES_PER_ROW;
    if(hex_viewer->model->read_bytes % HEX_VIEWER_BYTES_PER_ROW != 0) row_iters += 1;

    for(uint32_t i = 0; i < row_iters; ++i) {
        uint32_t bytes_left_per_row = hex_viewer->model->read_bytes - i * HEX_VIEWER_BYTES_PER_ROW;
        if(bytes_left_per_row > HEX_VIEWER_BYTES_PER_ROW)
            bytes_left_per_row = HEX_VIEWER_BYTES_PER_ROW;

        if(hex_viewer->model->mode) {
            memcpy(temp_buf, hex_viewer->model->file_bytes[i], bytes_left_per_row);
            temp_buf[bytes_left_per_row] = '\0';
            for(uint32_t j = 0; j < bytes_left_per_row; ++j)
                if(!isprint((int)temp_buf[j])) temp_buf[j] = '.';

            canvas_set_font(canvas, FontKeyboard);
            canvas_draw_str(canvas, 0, TOP_OFFSET + i * ROW_HEIGHT, temp_buf);
        } else {
            int addr = (i + hex_viewer->model->line) * HEX_VIEWER_BYTES_PER_ROW;
            snprintf(temp_buf, 32, "%04d", addr);

            canvas_set_font(canvas, FontKeyboard);
            canvas_draw_str(canvas, 0, TOP_OFFSET + i * ROW_HEIGHT, temp_buf);
        }

        char* p = temp_buf;
        for(uint32_t j = 0; j < bytes_left_per_row; ++j)
            p += snprintf(p, 32, "%02X ", hex_viewer->model->file_bytes[i][j]);

        canvas_set_font(canvas, FontKeyboard);
        canvas_draw_str(canvas, 40, TOP_OFFSET + i * ROW_HEIGHT, temp_buf);
    }

    furi_mutex_release(hex_viewer->model_mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    HexViewer* music_player = ctx;
    if(input_event->type == InputTypeShort) {
        furi_message_queue_put(music_player->input_queue, input_event, 0);
    }
}

HexViewer* hex_viewer_alloc() {
    HexViewer* instance = malloc(sizeof(HexViewer));

    instance->model = malloc(sizeof(HexViewerModel));
    // memset(instance->model->file_bytes, 0x0, HEX_VIEWER_BYTES_PER_ROW * HEX_VIEWER_ROW_COUNT);

    instance->model_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    instance->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    instance->view_port = view_port_alloc();
    view_port_draw_callback_set(instance->view_port, render_callback, instance);
    view_port_input_callback_set(instance->view_port, input_callback, instance);

    instance->gui = furi_record_open("gui");
    gui_add_view_port(instance->gui, instance->view_port, GuiLayerFullscreen);

    return instance;
}

void hex_viewer_free(HexViewer* instance) {
    gui_remove_view_port(instance->gui, instance->view_port);
    furi_record_close("gui");
    view_port_free(instance->view_port);

    furi_message_queue_free(instance->input_queue);

    furi_mutex_free(instance->model_mutex);

    free(instance->model);
    free(instance);
}

bool hex_viewer_read_file(HexViewer* hex_viewer, const char* file_path) {
    furi_assert(hex_viewer);
    furi_assert(file_path);

    // furi_check(furi_mutex_acquire(hex_viewer->model_mutex, FuriWaitForever) == FuriStatusOk);
    memset(hex_viewer->model->file_bytes, 0x0, HEX_VIEWER_ROW_COUNT * HEX_VIEWER_BYTES_PER_ROW);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    do {
        if(!storage_file_open(file, file_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_E(TAG, "Unable to open file");
            break;
        };

        uint32_t offset = hex_viewer->model->line * HEX_VIEWER_BYTES_PER_ROW;
        if(!storage_file_seek(file, offset, true)) {
            FURI_LOG_E(TAG, "Unable to seek file");
            break;
        }

        hex_viewer->model->read_bytes = storage_file_read(
            file, hex_viewer->model->file_bytes, HEX_VIEWER_ROW_COUNT * HEX_VIEWER_BYTES_PER_ROW);
    } while(false);

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    // furi_mutex_release(hex_viewer->model_mutex);

    return true;
}

int32_t hex_viewer_app(void* p) {
    HexViewer* hex_viewer = hex_viewer_alloc();

    FuriString* file_path;
    file_path = furi_string_alloc();

    do {
        if(p && strlen(p)) {
            furi_string_set(file_path, (const char*)p);
        } else {
            furi_string_set(file_path, HEX_VIEWER_APP_PATH_FOLDER);

            DialogsFileBrowserOptions browser_options;
            dialog_file_browser_set_basic_options(
                &browser_options, HEX_VIEWER_APP_EXTENSION, &I_hex_10px);
            browser_options.hide_ext = false;

            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
            bool res = dialog_file_browser_show(dialogs, file_path, file_path, &browser_options);

            furi_record_close(RECORD_DIALOGS);
            if(!res) {
                FURI_LOG_I(TAG, "No file selected");
                break;
            }
        }

        if(!hex_viewer_read_file(hex_viewer, furi_string_get_cstr(file_path))) {
            FURI_LOG_E(TAG, "Unable to load file: %s", furi_string_get_cstr(file_path));
            break;
        }

        InputEvent input;
        while(furi_message_queue_get(hex_viewer->input_queue, &input, FuriWaitForever) ==
              FuriStatusOk) {
            furi_check(
                furi_mutex_acquire(hex_viewer->model_mutex, FuriWaitForever) == FuriStatusOk);

            if(input.key == InputKeyBack) {
                furi_mutex_release(hex_viewer->model_mutex);
                break;
            } else if(input.key == InputKeyUp) {
                if(hex_viewer->model->line > 0) hex_viewer->model->line--;

                if(!hex_viewer_read_file(hex_viewer, furi_string_get_cstr(file_path))) {
                    FURI_LOG_E(TAG, "Unable to load file: %s", furi_string_get_cstr(file_path));
                    break;
                }
            } else if(input.key == InputKeyDown) {
                uint32_t max_bytes = HEX_VIEWER_ROW_COUNT * HEX_VIEWER_BYTES_PER_ROW;
                if(hex_viewer->model->read_bytes == max_bytes) hex_viewer->model->line++;

                if(!hex_viewer_read_file(hex_viewer, furi_string_get_cstr(file_path))) {
                    FURI_LOG_E(TAG, "Unable to load file: %s", furi_string_get_cstr(file_path));
                    break;
                }
            } else if(input.key == InputKeyLeft || input.key == InputKeyRight) {
                hex_viewer->model->mode = !hex_viewer->model->mode;
            }

            furi_mutex_release(hex_viewer->model_mutex);
            view_port_update(hex_viewer->view_port);
        }
    } while(false);

    furi_string_free(file_path);
    hex_viewer_free(hex_viewer);

    return 0;
}
