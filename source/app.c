#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__

#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#endif

#include "app.h"
#include "timing.h"

typedef enum {
    jsmpeg_frame_type_video = 0xFA010000,
    jsmpeg_frame_type_audio = 0xFB010000
} jsmpeg_trame_type_t;

typedef struct {
    jsmpeg_trame_type_t type;
    int size;
    char data[0];
} jsmpeg_frame_t;

typedef struct {
    unsigned char magic[4];
    unsigned short width;
    unsigned short height;
} jsmpeg_header_t;


typedef enum {
    input_type_key = 0x0001,
    input_type_mouse_button = 0x0002,
    input_type_mouse_absolute = 0x0004,
    input_type_mouse_relative = 0x0008,
    input_type_mouse =
    input_type_mouse_button |
    input_type_mouse_absolute |
    input_type_mouse_relative
} input_type_t;

typedef struct {
    unsigned short type;
    unsigned short state;
    unsigned short key_code;
} input_key_t;

typedef struct {
    unsigned short type;
    unsigned short flags;
    float x, y;
} input_mouse_t;


int swap_int32(int in) {
    return ((in >> 24) & 0xff) |
           ((in << 8) & 0xff0000) |
           ((in >> 8) & 0xff00) |
           ((in << 24) & 0xff000000);
}

int swap_int16(int in) {
    return ((in >> 8) & 0xff) | ((in << 8) & 0xff00);
}

// Proxies for app_on_* callbacks
void on_connect(server_t *server, struct lws *socket) { app_on_connect((app_t *) server->user, socket); }

void on_message(server_t *server, struct lws *socket, void *data, size_t len) {
    app_on_message((app_t *) server->user, socket, data, len);
}

void on_close(server_t *server, struct lws *socket) { app_on_close((app_t *) server->user, socket); }


app_t *app_create(window_system_connection_t *window_system_connection, window_t *window,
                  int port, int bit_rate, int out_width, int out_height, int allow_input,
                  grabber_crop_area_t crop) {
    app_t *self = (app_t *) malloc(sizeof(app_t));
    memset(self, 0, sizeof(app_t));

    self->window_system_connection = window_system_connection;
    self->mouse_speed = APP_MOUSE_SPEED;
    self->grabber = grabber_create(window_system_connection, window, crop);
    self->allow_input = allow_input;

    if (!out_width) { out_width = self->grabber->width; }
    if (!out_height) { out_height = self->grabber->height; }
    if (!bit_rate) { bit_rate = out_width * 1500; } // estimate bit rate based on output size

    self->encoder = encoder_create(
        self->grabber->width, self->grabber->height, // in size
        out_width, out_height, // out size
        bit_rate
    );

    self->server = server_create(port, APP_FRAME_BUFFER_SIZE);
    if (!self->server) {
        printf("Error: could not create Server; try using another port\n");
        return NULL;
    }

    self->server->on_connect = on_connect;
    self->server->on_message = on_message;
    self->server->on_close = on_close;
    self->server->user = self; // Set the app as user data, so we can access it in callbacks

    return self;
}

void app_destroy(app_t *self) {
    if (self == NULL) { return; }

    encoder_destroy(self->encoder);
    grabber_destroy(self->grabber);
    server_destroy(self->server);
    free(self);
}

void app_on_connect(app_t *self, struct lws *socket) {
    printf("\nclient connected: %s\n", server_get_client_address(self->server, socket));

    jsmpeg_header_t header = {
        {'j', 's', 'm', 'p'},
        swap_int16(self->encoder->out_width), swap_int16(self->encoder->out_height)
    };
    server_send(self->server, socket, &header, sizeof(header), LWS_WRITE_BINARY);
}

void app_on_close(app_t *self, struct lws *socket) {
    printf("\nclient disconnected: %s\n", server_get_client_address(self->server, socket));
}

void app_on_message(app_t *self, struct lws *socket, void *data, size_t len) {
    if (!self->allow_input) {
        return;
    }

    input_type_t type = (input_type_t) ((unsigned short *) data)[0];

    if (type & input_type_key && len >= sizeof(input_key_t)) {
        input_key_t *input = (input_key_t *) data;

        if (input->key_code == /* VK_CAPITAL */ 0x14) { return; } // ignore caps lock
#ifdef __linux__
        KeySym key_sym;
        switch (input->key_code) {
            case 0x11:
                key_sym = XK_Control_L;
                break;
            case 0x10:
                key_sym = XK_Shift_L;
                break;
            case 0x12:
                key_sym = XK_Alt_L;
                break;
            case 0x25:
                key_sym = XK_Left;
                break;
            case 0x26:
                key_sym = XK_Up;
                break;
            case 0x28:
                key_sym = XK_Down;
                break;
            case 0x27:
                key_sym = XK_Right;
                break;
            case 0x2E:
                key_sym = XK_Delete;
                break;
            case 0x8:
                key_sym = XK_BackSpace;
                break;
            case 0xD:
                key_sym = XK_Return;
                break;
            default:
                key_sym = input->key_code;
                break;
        }
        KeyCode key_code = XKeysymToKeycode(
            window_system_connection_get_display(self->window_system_connection),
            key_sym
        );
        printf("key: %d -> %d\n", input->key_code, (unsigned int) key_code);
        if (key_code != 0) {
            XTestFakeKeyEvent(
                window_system_connection_get_display(self->window_system_connection),
                key_code,
                input->state,
                0
            );
        }
#endif
#ifdef WIN32
        UINT scan_code = MapVirtualKey(input->key_code, MAPVK_VK_TO_VSC);
        UINT flags = KEYEVENTF_SCANCODE | (input->state ? 0 : KEYEVENTF_KEYUP);

        // set extended bit for some keys
        switch (input->key_code) {
            case VK_LEFT:
            case VK_UP:
            case VK_RIGHT:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_END:
            case VK_HOME:
            case VK_INSERT:
            case VK_DELETE:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                scan_code |= 0x100;
                flags |= KEYEVENTF_EXTENDEDKEY;
                break;
        }

        printf("key: %d -> %d\n", input->key_code, scan_code);
        keybd_event((BYTE) input->key_code, scan_code, flags, 0);
#endif
    } else if (type & input_type_mouse && len >= sizeof(input_mouse_t)) {
        input_mouse_t *input = (input_mouse_t *) data;

        if (type & input_type_mouse_absolute) {
            float scale_x = ((float) self->encoder->in_width / self->encoder->out_width);
            float scale_y = ((float) self->encoder->in_height / self->encoder->out_height);
#ifdef __linux__
            int x = (int) (input->x * scale_x);
            int y = (int) (input->y * scale_y);
            printf("mouse absolute %d, %d\n", x, y);
            XWarpPointer(
                window_system_connection_get_display(self->window_system_connection),
                window_get_handle(self->grabber->window),
                window_get_handle(self->grabber->window),
                0,
                0,
                0,
                0,
                x,
                y
            );
#endif
#ifdef WIN32
            POINT window_pos = {0, 0};
            ClientToScreen(self->grabber->window, &window_pos);
            int x = (int) (input->x * scale_x + window_pos.x + self->grabber->crop.x);
            int y = (int) (input->y * scale_y + window_pos.y + self->grabber->crop.y);
            printf("mouse absolute %d, %d\n", x, y);
            SetCursorPos(x, y);
#endif
        }

        if (type & input_type_mouse_relative) {
            int x = (int) (input->x * self->mouse_speed);
            int y = (int) (input->y * self->mouse_speed);
            printf("mouse relative %d, %d\n", x, y);
#ifdef WIN32
            mouse_event(MOUSEEVENTF_MOVE, x, y, 0, 0);
#endif
        }

        if (type & input_type_mouse_button) {
            printf("mouse button %d\n", input->flags);
#ifdef __linux__
            XTestFakeButtonEvent(
                window_system_connection_get_display(self->window_system_connection),
                input->flags & 0x6 ? 1 : 3,
                (input->flags & 0xA) != 0,
                CurrentTime
            );
#endif
#ifdef WIN32
            mouse_event(input->flags, 0, 0, 0, 0);
#endif
        }
    }
}

void app_run(app_t *self, int target_fps) {
    jsmpeg_frame_t *frame = (jsmpeg_frame_t *) malloc(APP_FRAME_BUFFER_SIZE);
    frame->type = jsmpeg_frame_type_video;
    frame->size = 0;

    double
        fps = 60.0f,
        wait_time = (1000.0f / target_fps) - 1.5f;

    double frame_start_time = timing_get_current_milliseconds();

    while (!interrupted) {
        double delta = timing_get_current_milliseconds() - frame_start_time;
        if (delta > wait_time) {
            fps = fps * 0.95f + 50.0f / delta;
            frame_start_time = timing_get_current_milliseconds();

            void *pixels;
            double grab_start_time = timing_get_current_milliseconds();
            pixels = grabber_grab(self->grabber, self->window_system_connection);
            double grab_time = timing_get_current_milliseconds() - grab_start_time;

            double encode_start_time = timing_get_current_milliseconds();
            size_t encoded_size = APP_FRAME_BUFFER_SIZE - sizeof(jsmpeg_frame_t);
            encoder_encode(self->encoder, pixels, frame->data, &encoded_size);
            if (encoded_size) {
                frame->size = swap_int32(sizeof(jsmpeg_frame_t) + encoded_size);
                server_broadcast(self->server, frame, sizeof(jsmpeg_frame_t) + encoded_size, LWS_WRITE_BINARY);
            }
            double encode_time = timing_get_current_milliseconds() - encode_start_time;

            printf("fps:%3d (grabbing:%6.2fms, scaling/encoding:%6.2fms)\r", (int) fps, grab_time, encode_time);
            fflush(stdout);
        }

        server_update(self->server);
#ifndef WIN32
        usleep(1000);
#else
        Sleep(1);
#endif
    }

    free(frame);
}
