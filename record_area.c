#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pwd.h>
#include <sys/stat.h>
#include <dbus-1.0/dbus/dbus.h>
#include <gstreamer-1.0/gst/gst.h>
#include <gstreamer-1.0/gst/gstparse.h>
#include <gstreamer-1.0/gst/gstelement.h>
#include <gstreamer-1.0/gst/gstmessage.h>
#include <unistd.h>

#include "raylib.h"

#define MOUSE_SCALE_MARK_SIZE  24

#define SCREENCAST_SERVICE "org.gnome.Mutter.ScreenCast"
#define SCREENCAST_OBJECT_PATH "/org/gnome/Mutter/ScreenCast"
#define SCREENCAST_INTERFACE "org.gnome.Mutter.ScreenCast"
#define SESSION_INTERFACE "org.gnome.Mutter.ScreenCast.Session"
#define STREAM_INTERFACE "org.gnome.Mutter.ScreenCast.Stream"

typedef struct {
    DBusConnection *conn;
    char *session_path;
    char *stream_path;
    unsigned int pipewire_node_id;
} ScreenCastState;

typedef struct {
    gboolean is_live;
    GstElement *pipeline;
    GMainLoop *loop;
} CustomData;

enum OutputEncoding {
    WEBM_WITH_AUDIO,
    WEBM_ONLY_VIDEO,
    GIF
};

bool received_eos = false;

typedef struct {
    bool show_debug_info;

    bool is_resizing_recording_area;
    bool is_recording;

    enum OutputEncoding output_encoding;
} UISettings;

const char * const  PIPELINES[] = {
    "webmmux name=mux ! filesink location=%s "
    "pipewiresrc path=%u \
        do-timestamp=true \
        keepalive-time=1000 \
        resend-last=true ! "
    "capsfilter caps=video/x-raw,max-framerate=30/1 ! "
    "videoconvert matrix-mode=output-only n-threads=32 ! "
    "queue ! "
    "vp8enc cpu-used=16 max-quantizer=17 deadline=1 keyframe-mode=disabled threads=32 static-threshold=100 buffer-size=20000 ! "
    "queue ! "
    "mux.video_0 "
    "pulsesrc ! audioconvert ! vorbisenc ! queue ! mux.audio_0",

    "pipewiresrc path=%u \
        do-timestamp=true \
        keepalive-time=1000 \
        resend-last=true ! "
    "capsfilter caps=video/x-raw,max-framerate=30/1 ! "
    "videoconvert chroma-mode=none dither=none matrix-mode=output-only n-threads=32 ! "
    "queue ! "
    "vp8enc cpu-used=16 max-quantizer=17 deadline=1 keyframe-mode=disabled threads=32 static-threshold=1000 buffer-size=20000 ! "
    "queue ! "
    "webmmux ! filesink location=%s",

    "pipewiresrc path=%u \
        do-timestamp=true \
        keepalive-time=1000 \
        resend-last=true ! "
    "capsfilter caps=video/x-raw,max-framerate=60/1 ! "
    "videoconvert chroma-mode=none dither=none matrix-mode=output-only n-threads=32 ! "
    "queue ! "
    "gifskienc quality=100 location=%s ! fakesink",
};

#define INITIAL_RECORDING_AREA_X 300
#define INITIAL_RECORDING_AREA_Y 100

static CustomData data;
static UISettings ui_settings;

bool create_screen_cast_session(DBusConnection **conn, char **session_path)
{
    DBusError err;
    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        SCREENCAST_OBJECT_PATH,
        SCREENCAST_INTERFACE,
        "CreateSession"
    );

    if (msg == NULL) {
        fprintf(stderr, "ERROR: Message Null (CreateSession)\n");

        return false;
    }

    // Add the 'properties' argument (empty dictionary for now)
    DBusMessageIter arg;
    dbus_message_iter_init_append(msg, &arg);
    DBusMessageIter dict_iter;
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    dbus_message_iter_close_container(&arg, &dict_iter);

    // Send the message and get the reply
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Error calling CreateSession: %s\n", err.message);

        dbus_error_free(&err);
        dbus_message_unref(reply);
        return false;
    }

    // Get the session_path from the reply
    if (reply) {
        char *session_path_reply;

        if (dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &session_path_reply, DBUS_TYPE_INVALID)) {
            *session_path = strdup(session_path_reply);
            printf("INFO: Created session at: %s\n", *session_path);

            dbus_message_unref(reply);
            return true;
        }

        fprintf(stderr, "ERROR: Error getting session_path: %s\n", err.message);
        dbus_error_free(&err);
        dbus_message_unref(reply);

        return false;
    }

    fprintf(stderr, "ERROR: Reply for CreateSession is NULL\n");

    dbus_error_free(&err);
    return false;
}
bool start_screen_cast_session(DBusConnection **conn, char **session_path)
{
    DBusError err;
    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        *session_path,
        SESSION_INTERFACE,
        "Start"
    );

    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Error calling Session.Start: %s\n", err.message);
        dbus_error_free(&err);

        return false;
    }

    if (!reply) {
        fprintf(stderr, "ERROR: Reply for Session.Start is NULL\n");

        return false;
    }

    dbus_message_unref(reply);
    printf("INFO: Session at [%s] started successfully.\n", *session_path);

    return true;
}
void handle_pipewire_stream_added(unsigned int *pipewire_node_id, DBusMessage * signal) {
    DBusError err;
    dbus_error_init(&err);
    unsigned int node_id;

    if (!dbus_message_get_args(signal, &err, DBUS_TYPE_UINT32, &node_id, DBUS_TYPE_INVALID)) {
        fprintf(stderr, "ERROR: Error getting PipeWireStreamAdded arguments: %s\n", err.message);
        dbus_error_free(&err);

        return;
    }

    *pipewire_node_id = node_id;
    printf("INFO: PipeWire stream added with node ID: %u\n", node_id);
}
DBusHandlerResult filter_function(DBusConnection * conn, DBusMessage * msg, void *user_data) {
    unsigned int *pipewire_node_id = user_data;

    if (dbus_message_is_signal(msg, STREAM_INTERFACE, "PipeWireStreamAdded")) {
        handle_pipewire_stream_added(pipewire_node_id, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
void subscribe_to_pipewire_added_event(DBusConnection **conn, unsigned int *pipewire_node_id)
{
    dbus_bus_add_match(*conn, "type='signal',interface='" STREAM_INTERFACE "',member='PipeWireStreamAdded'", nullptr);
    dbus_connection_add_filter(*conn, filter_function, pipewire_node_id, nullptr);
    dbus_connection_flush(*conn);
}
bool create_stream_cast_record_area_stream(DBusConnection **conn, char **session_path, char **stream_path, const int x, const int y, const int width, const int height)
{
    DBusError err;

    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        *session_path,
        SESSION_INTERFACE,
        "RecordArea"
    );

    if (msg == NULL) {
        fprintf(stderr, "ERROR: Message Null (RecordArea)\n");

        return false;
    }

    const int cursor_mode_value = 2;
    const char *cursor_mode_key = "cursor-mode";

    DBusMessageIter arg;
    DBusMessageIter dict_iter;
    DBusMessageIter entry_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_init_append(msg, &arg);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_INT32, &x);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_INT32, &y);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_INT32, &width);
    dbus_message_iter_append_basic(&arg, DBUS_TYPE_INT32, &height);
    // Add the 'properties' argument (dictionary)
    dbus_message_iter_open_container(&arg, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry_iter);
            dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &cursor_mode_key);
            dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "u", &variant_iter);
                dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_UINT32, &cursor_mode_value);
            dbus_message_iter_close_container(&entry_iter, &variant_iter);
        dbus_message_iter_close_container(&dict_iter, &entry_iter);
    dbus_message_iter_close_container(&arg, &dict_iter);

    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "Error calling RecordArea: %s\n", err.message);

        dbus_error_free(&err);
        return false;
    }

    if (!reply) {
        fprintf(stderr, "ERROR: Reply for RecordArea is NULL\n");

        return false;
    }

    char *stream_path_reply;
    if (dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &stream_path_reply, DBUS_TYPE_INVALID)) {
        *stream_path = strdup(stream_path_reply);
        printf("INFO: Started recording area, stream at: %s\n", *stream_path);
    } else {
        fprintf(stderr, "ERROR: Error getting stream_path: %s\n", err.message);
        dbus_error_free(&err);

        return false;
    }

    dbus_message_unref(reply);

    return true;
}
bool start_screen_cast_record_area_stream(DBusConnection **conn, char **stream_path)
{
    DBusError err;

    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        *stream_path,
        STREAM_INTERFACE,
        "Start"
    );

    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Error starting stream [%s]: %s\n", *stream_path, err.message);
        dbus_error_free(&err);

        return false;
    }

    if (!reply) {
        fprintf(stderr, "ERROR: No reply when starting record area stream\n");
    }

    dbus_message_unref(reply);
    printf("INFO: Record area stream started successfully.\n");

    return true;
}
bool stop_screen_cast_record_area_stream(DBusConnection **conn, char **stream_path)
{
    DBusError err;

    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        *stream_path,
        STREAM_INTERFACE,
        "Stop"
    );

    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Error stopping stream [%s]: %s\n", *stream_path, err.message);
        dbus_error_free(&err);

        return false;
    }

    if (!reply) {
        fprintf(stderr, "ERROR: No reply when stopping record area stream\n");
    }

    dbus_message_unref(reply);
    printf("INFO: Record area stream stopped successfully.\n");

    return true;
}
bool stop_screen_cast_session(DBusConnection **conn, char **session_path)
{
    DBusError err;

    DBusMessage *msg = dbus_message_new_method_call(
        SCREENCAST_SERVICE,
        *session_path,
        SESSION_INTERFACE,
        "Stop"
    );

    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(*conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Error stopping session [%s]: %s\n", *session_path, err.message);
        dbus_error_free(&err);

        return false;
    }

    if (!reply) {
        fprintf(stderr, "ERROR: No reply when stopping session\n");
    }

    dbus_message_unref(reply);
    printf("INFO: Session stopped successfully.\n");

    return true;
}

static void cb_message(GstBus *bus, GstMessage *msg, const CustomData *data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            char *debug;

            gst_message_parse_error(msg, &err, &debug);
            g_print("Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug);

            gst_element_set_state(data->pipeline, GST_STATE_READY);
            break;
        }
        case GST_MESSAGE_EOS:
            received_eos = true;
            gst_element_set_state(data->pipeline, GST_STATE_READY);
            break;
        case GST_MESSAGE_BUFFERING: {
            gint percent = 0;

            /* If the stream is live, we do not care about buffering. */
            if (data->is_live) break;

            gst_message_parse_buffering(msg, &percent);

            if (percent < 100)
                gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
            else
                gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
            break;
        }
        case GST_MESSAGE_CLOCK_LOST:
            /* Get a new clock */
            gst_element_set_state(data->pipeline, GST_STATE_PAUSED);
            gst_element_set_state(data->pipeline, GST_STATE_PLAYING);
            break;
        default:
            printf("DEBUG: Message type: %s\n", gst_message_type_get_name(GST_MESSAGE_TYPE(msg)));
            break;
    }
}

static void get_pipeline_string(char *str, const unsigned int pipewire_node_id)
{
    const char *homedir;

    if ((homedir = getenv("HOME")) == NULL) {
        homedir = getpwuid(getuid())->pw_dir;
    }

    char format_string[100];
    char full_path[100];

    const time_t t = time(nullptr);
    const struct tm *tm = localtime(&t);

    snprintf(format_string, sizeof(format_string), "%s/Videos/Screencasts/screen_cast_%%d_%%m_%%Y_%%H_%%M_%%S", homedir);

    strftime(full_path, sizeof(full_path), format_string, tm);

    switch (ui_settings.output_encoding) {
        case WEBM_WITH_AUDIO:
            strcat(full_path, ".webm");
            sprintf(str, PIPELINES[WEBM_WITH_AUDIO], full_path, pipewire_node_id);
            break;
        case WEBM_ONLY_VIDEO:
            strcat(full_path, ".webm");
            sprintf(str, PIPELINES[WEBM_ONLY_VIDEO], pipewire_node_id, full_path);
            break;
        case GIF:
            strcat(full_path, ".gif");
            sprintf(str, PIPELINES[GIF], pipewire_node_id, full_path);
            break;
    }
}

GstElement* create_pipeline(const unsigned int pipewire_node_id)
{
    char fullPipeline[9999];
    get_pipeline_string(fullPipeline, pipewire_node_id);

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(fullPipeline, &error);

    if (pipeline == NULL) {
        fprintf(stderr, "ERROR: Failed to create pipeline\n");
        return nullptr;
    }

    if (error != NULL) {
        fprintf(stderr, "ERROR: Error parsing full pipeline: %d, %s\n", error->code, error->message);
        g_error_free(error);
        return nullptr;
    }

    return pipeline;
}

bool is_gifskienc_plugin_loaded(void)
{
    GstRegistry *registry = gst_registry_get();

    const GstPlugin *plugin = gst_registry_find_plugin(registry, "gifskienc");

    return plugin != NULL;
}

bool check_for_exit_flag(void) {
    FILE *f = fopen("/tmp/recording-indicator/flag.txt", "r");

    if (f != NULL) {
        fclose(f);
        return true;
    }

    return false;
}

int main(int argc, char *argv[])
{
    struct stat st = {0};

    if (stat("/tmp/recording-indicator", &st) == -1) {
        mkdir("/tmp/recording-indicator", 0777);
    }

    // Remove any existing indicator file
    remove("/tmp/recording-indicator/flag.txt");

    DBusError err;
    dbus_error_init(&err);

    ScreenCastState state = {};
    memset(&state, 0, sizeof(ScreenCastState));

    gst_init(nullptr, nullptr);

    memset(&data, 0, sizeof(data));

    if (argc == 2) {
        if (strcmp(argv[1], "GIF") == 0) {
            if (!is_gifskienc_plugin_loaded()) {
                fprintf(stderr, "ERROR: gifskienc plugin is needed to use GIF encoding.\n");
                return 1;
            }

            ui_settings.output_encoding = GIF;
        } else if (strcmp(argv[1], "WEBM_WITH_AUDIO") == 0) {
            ui_settings.output_encoding = WEBM_WITH_AUDIO;
        } else {
            ui_settings.output_encoding = WEBM_ONLY_VIDEO;
        }
    }

    ui_settings.show_debug_info = false;
    ui_settings.is_resizing_recording_area = true;
    ui_settings.is_recording = false;

    // Connect to the session bus
    state.conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "ERROR: Connection Error (%s)\n", err.message);

        dbus_error_free(&err);
        return 1;
    }

    if (state.conn == NULL) {
        fprintf(stderr, "ERROR: Connection Null\n");

        dbus_error_free(&err);
        return 1;
    }

    if (! create_screen_cast_session(&state.conn, &state.session_path)) {
      	dbus_connection_unref(state.conn);

    	return 1;
    }

    if (! start_screen_cast_session(&state.conn, &state.session_path)) {
      	dbus_connection_unref(state.conn);

    	return 1;
    }

    subscribe_to_pipewire_added_event(&state.conn, &state.pipewire_node_id);

    SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_TRANSPARENT | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST);
    InitWindow(0, 0, "");

    int monitor = GetCurrentMonitor();
    float screenWidth = (float)GetScreenWidth() * GetWindowScaleDPI().x;
    float screenHeight = (float)GetScreenHeight() * GetWindowScaleDPI().y;
    const float monitorPositionX = GetMonitorPosition(monitor).x;

    SetWindowPosition((int)monitorPositionX, 0);
    SetWindowSize((int)screenWidth, (int)screenHeight);
    SetTargetFPS(60);

    Rectangle rec = { (screenWidth / 2) - INITIAL_RECORDING_AREA_X, (screenHeight / 2) - INITIAL_RECORDING_AREA_Y, INITIAL_RECORDING_AREA_X, INITIAL_RECORDING_AREA_Y };
    Vector2 mousePosition = { 0 };

    bool mouseScaleReady = false;
    bool mouseMoveMode = false;

    bool isInTopLeftCorner = false;
    bool isInBottomRightCorner = false;
    bool isInTopRightCorner = false;
    bool isInBottomLeftCorner = false;

    int scaleCorner = 0; // 1: topLeft, 2: bottomRight, 3: topRight, 4: bottomLeft

    Color backgroundColor = Fade(BLACK, 0.8f);
    Color scaleMarkColor = LIGHTGRAY;
    Color activeScaleMarkColor = GRAY;

    double startTime = 0.0;
    int elapsedSeconds = 0;

    while (!WindowShouldClose())
    {
        if (ui_settings.is_recording) {
            elapsedSeconds = (int)(GetTime() - startTime);

            if (check_for_exit_flag()) {
                printf("INFO: Finishing recording...\n");
                remove("/tmp/recording-indicator/flag.txt");
                break;
            }
        }

      	if (IsKeyPressed(KEY_D)) {
      	    ui_settings.show_debug_info = !ui_settings.show_debug_info;
        }

        mousePosition = GetMousePosition();

        isInTopLeftCorner = CheckCollisionPointCircle(mousePosition, (Vector2){ rec.x, rec.y }, MOUSE_SCALE_MARK_SIZE);
        isInBottomRightCorner = CheckCollisionPointCircle(mousePosition, (Vector2){ rec.x + rec.width, rec.y + rec.height }, MOUSE_SCALE_MARK_SIZE);
        isInTopRightCorner = CheckCollisionPointCircle(mousePosition, (Vector2){ rec.x + rec.width, rec.y }, MOUSE_SCALE_MARK_SIZE);
        isInBottomLeftCorner = CheckCollisionPointCircle(mousePosition, (Vector2){ rec.x, rec.y + rec.height }, MOUSE_SCALE_MARK_SIZE);

        if (ui_settings.is_recording == false && !ui_settings.is_resizing_recording_area && (isInTopLeftCorner || isInBottomRightCorner || isInTopRightCorner || isInBottomLeftCorner)) {
            mouseScaleReady = true;
             if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                 ui_settings.is_resizing_recording_area = true;
                 if (isInTopLeftCorner) scaleCorner = 1;
                 else if (isInBottomRightCorner) scaleCorner = 2;
                 else if (isInTopRightCorner) scaleCorner = 3;
                 else if(isInBottomLeftCorner) scaleCorner = 4;
             }
        } else if (!ui_settings.is_resizing_recording_area) {
            mouseScaleReady = false;
        }

        if (ui_settings.is_resizing_recording_area) {
             mouseScaleReady = true;

            // Change rectangle size based on mouse position. Mouse can be on any 4 corners of the rectangle
            // TODO: check for rec.x = 0 or rec.y = 0 cases
            if (scaleCorner == 1) {
               rec.width = rec.x + rec.width - mousePosition.x;
               rec.height = rec.y + rec.height - mousePosition.y;
               rec.x = mousePosition.x;
               rec.y = mousePosition.y;
            } else if (scaleCorner == 2) {
               rec.width = mousePosition.x - rec.x;
               rec.height = mousePosition.y - rec.y;
            } else if (scaleCorner == 3) {
               rec.width = mousePosition.x - rec.x;
               rec.height = rec.y + rec.height - mousePosition.y;
               rec.y = mousePosition.y;
            } else if (scaleCorner == 4) {
               rec.width = rec.x + rec.width - mousePosition.x;
               rec.height = mousePosition.y - rec.y;
               rec.x = mousePosition.x;
            }

            // Check out of bounds
            if (rec.x < 0) rec.x = 0;
            if (rec.y < 0) rec.y = 0;

            // Check minimum rec size
            if (rec.width < MOUSE_SCALE_MARK_SIZE * 2) rec.width = MOUSE_SCALE_MARK_SIZE * 2;
            if (rec.height < MOUSE_SCALE_MARK_SIZE * 2) rec.height = MOUSE_SCALE_MARK_SIZE * 2;

            // Check maximum rec size
            if (rec.width > ((float)screenWidth - rec.x)) rec.width = (float)screenWidth - rec.x;
            if (rec.height > ((float)screenHeight - rec.y)) rec.height = (float)screenHeight - rec.y;

			if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
    			ui_settings.is_resizing_recording_area = false;

    			mouseScaleReady = false;
    			scaleCorner = 0;
			}
        }

        // Move recording area rectangle with right mouse click
        if (ui_settings.is_recording == false && (CheckCollisionPointRec(mousePosition, rec) || mouseMoveMode) && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            ui_settings.is_resizing_recording_area = false;
            mouseScaleReady = false;
            mouseMoveMode = true;
            rec.x = mousePosition.x - rec.width / 2;
            rec.y = mousePosition.y - rec.height / 2;

            // Check out of bounds
            if (rec.x < 0) rec.x = 0;
            if (rec.y < 0) rec.y = 0;
            if (rec.x + rec.width > screenWidth) rec.x = screenWidth - rec.width;
            if (rec.y + rec.height > screenHeight) rec.y = screenHeight - rec.height;
        }

        if ((CheckCollisionPointRec(mousePosition, rec) || mouseMoveMode) && IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
            mouseMoveMode = false;
        }

        BeginDrawing();
        ClearBackground(BLANK);

        if (ui_settings.show_debug_info) {
            DrawRectangle(0, 0, (int)screenWidth, (int)rec.y, RED);
            DrawRectangle(0, (int)rec.y + (int)rec.height, (int)screenWidth, (int)screenHeight - (int)(rec.y + rec.height), GREEN);
            DrawRectangle(0, (int)rec.y, (int)rec.x, (int)rec.height, BLUE);
            DrawRectangle((int)rec.x + (int)rec.width, (int)rec.y, (int)screenWidth - (int)(rec.x + rec.width), (int)rec.height, YELLOW);

            DrawText(TextFormat("Rectangle X: %03f", rec.x), (int)screenWidth - 970, 100, 60, WHITE);
            DrawText(TextFormat("Rectangle Y: %03f", rec.y), (int)screenWidth - 970, 200, 60, WHITE);
            DrawText(TextFormat("Rectangle Width: %03f", rec.width), (int)screenWidth - 970, 300, 60, WHITE);
            DrawText(TextFormat("Rectangle Height: %03f", rec.height), (int)screenWidth - 970, 400, 60, WHITE);
            DrawText(TextFormat("Mouse position x: %03f", mousePosition.x), (int)screenWidth - 970, 500, 60, WHITE);
            DrawText(TextFormat("Mouse position y: %03f", mousePosition.y), (int)screenWidth - 970, 600, 60, WHITE);
        } else {
            DrawRectangle(0, 0, (int)screenWidth, (int)rec.y, backgroundColor);
            DrawRectangle(0, (int)rec.y + (int)rec.height, (int)screenWidth, (int)screenHeight - (int)(rec.y + rec.height), backgroundColor);
            DrawRectangle(0, (int)rec.y, (int)rec.x, (int)rec.height, backgroundColor);
            DrawRectangle((int)rec.x + (int)rec.width, (int)rec.y, (int)screenWidth - (int)(rec.x + rec.width), (int)rec.height, backgroundColor);
        }

        if (ui_settings.is_recording) {
            DrawRectangle((int)(screenWidth / 2) - 50, 0, 100, 50, RED);
            DrawText(TextFormat("%d", elapsedSeconds),(int)screenWidth / 2, 0,40,WHITE);
        }

        DrawRectangleRec(rec, BLANK);

        Color currentScaleMarkColor = scaleMarkColor;

        if (mouseScaleReady) currentScaleMarkColor = activeScaleMarkColor;

        // Draw rectangle scale marks
        if (ui_settings.is_recording == false) {
            DrawRectangleLinesEx(rec, 2, LIGHTGRAY);

            DrawCircle((int)rec.x, (int)rec.y, (float)MOUSE_SCALE_MARK_SIZE / 2, currentScaleMarkColor);
            DrawCircleLines((int)rec.x, (int)rec.y, (float)MOUSE_SCALE_MARK_SIZE / 2, BLACK);
            DrawCircle((int)rec.x + (int)rec.width, (int)rec.y + (int)rec.height, (float)MOUSE_SCALE_MARK_SIZE / 2, currentScaleMarkColor);
            DrawCircleLines((int)rec.x + (int)rec.width, (int)rec.y + (int)rec.height, (float)MOUSE_SCALE_MARK_SIZE / 2, BLACK);
            DrawCircle((int)rec.x + (int)rec.width, (int)rec.y, (float)MOUSE_SCALE_MARK_SIZE / 2, currentScaleMarkColor);
            DrawCircleLines((int)rec.x + (int)rec.width, (int)rec.y, (float)MOUSE_SCALE_MARK_SIZE / 2, BLACK);
            DrawCircle((int)rec.x, (int)rec.y + (int)rec.height, (float)MOUSE_SCALE_MARK_SIZE / 2, currentScaleMarkColor);
            DrawCircleLines((int)rec.x, (int)rec.y + (int)rec.height, (float)MOUSE_SCALE_MARK_SIZE / 2, BLACK);
        }

        if (ui_settings.is_recording == false) {
            // Draw recording indicator
            DrawRectangleRounded((Rectangle) { screenWidth / 2.0f - 75, screenHeight - 220, 150, 150 }, 0.3f, 0, Fade(WHITE, 0.8f));
            DrawCircle((int)(screenWidth / 2), (int)screenHeight - 145, 45, Fade(RED, 0.9f));

            if (CheckCollisionPointCircle(mousePosition, (Vector2){ screenWidth / 2, (float)screenHeight - 145 }, 45)) {
                DrawCircleLines((int)(screenWidth / 2), (int)screenHeight - 145, 45, BLACK);
            }

            if (ui_settings.is_recording == false && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointCircle(mousePosition, (Vector2){ screenWidth / 2.f, (float)screenHeight - 145 }, 45)) {
                SetWindowState(FLAG_WINDOW_MOUSE_PASSTHROUGH);

                startTime = GetTime();
                elapsedSeconds = 0;

                int y = (int)roundf(rec.y);
#ifdef GNOME_TOP_BAR
                y += GNOME_TOP_BAR;
#endif

                create_stream_cast_record_area_stream(
                    &state.conn, &state.session_path, &state.stream_path,
                    (int)roundf(rec.x+monitorPositionX), y,
                    (int)rec.width, (int)rec.height
                );

                start_screen_cast_record_area_stream(&state.conn, &state.stream_path);

                while (state.pipewire_node_id == 0) {
                    dbus_connection_read_write_dispatch(state.conn, -1);
                }

                data.pipeline = create_pipeline(state.pipewire_node_id);
                if (data.pipeline == NULL) {
                    break;
                }

                GstBus *bus = gst_element_get_bus(data.pipeline);

                /* Start playing */
                GstStateChangeReturn ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
                if (ret == GST_STATE_CHANGE_FAILURE) {
                    fprintf(stderr, "ERROR: Unable to set the pipeline to the playing state.\n");
                    return 1;
                }

                ui_settings.is_recording = true;
                gst_bus_add_signal_watch(bus);
                g_signal_connect(bus, "message", G_CALLBACK(cb_message), &data);
            }
        }

        EndDrawing();
    }

    if(data.pipeline) {
        GstBus *bus = gst_element_get_bus(data.pipeline);
        GstEvent *eos = gst_event_new_eos();
        gst_element_send_event(data.pipeline, eos);
        gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
        gst_element_set_state(data.pipeline, GST_STATE_NULL);
        gst_object_unref(data.pipeline);
        gst_event_unref(eos);
    }

    if (state.stream_path) {
        stop_screen_cast_record_area_stream(&state.conn, &state.stream_path);

        free(state.stream_path);
    }

    if (state.session_path) {
        stop_screen_cast_session(&state.conn, &state.session_path);

        free(state.session_path);
    }

    if (state.conn) dbus_connection_unref(state.conn);
    gst_deinit();

    CloseWindow();

    return 0;
}
