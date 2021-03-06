/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <cutils/android_reboot.h>
#include <cutils/properties.h>

#include "common.h"
#include "roots.h"
#include "device.h"
#include "minui/minui.h"
#include "screen_ui.h"
#include "ui.h"

#include "voldclient/voldclient.h"

#include "messagesocket.h"

#define UI_WAIT_KEY_TIMEOUT_SEC    120

// There's only (at most) one of these objects, and global callbacks
// (for pthread_create, and the input event system) need to find it,
// so use a global variable.
static RecoveryUI* self = NULL;

static int string_split(char* s, char** fields, int maxfields)
{
    int n = 0;
    while (n+1 < maxfields) {
        char* p = strchr(s, ' ');
        if (!p)
            break;
        *p = '\0';
        printf("string_split: field[%d]=%s\n", n, s);
        fields[n++] = s;
        s = p+1;
    }
    fields[n] = s;
    printf("string_split: last field[%d]=%s\n", n, s);
    return n+1;
}

static int message_socket_client_event(int fd, short revents, void *data)
{
    MessageSocket* client = (MessageSocket*)data;

    printf("message_socket client event\n");
    if (!(revents & POLLIN)) {
        return 0;
    }

    char buf[256];
    ssize_t nread;
    nread = client->Read(buf, sizeof(buf));
    if (nread <= 0) {
        ev_del_fd(fd);
        self->DialogDismiss();
        client->Close();
        delete client;
        return 0;
    }

    printf("message_socket client message <%s>\n", buf);

    // Parse the message.  Right now we support:
    //   dialog show <string>
    //   dialog dismiss
    char* fields[3];
    int nfields;
    nfields = string_split(buf, fields, 3);
    printf("fields=%d\n", nfields);
    if (nfields < 2)
        return 0;
    printf("field[0]=%s, field[1]=%s\n", fields[0], fields[1]);
    if (strcmp(fields[0], "dialog") == 0) {
        if (strcmp(fields[1], "show") == 0 && nfields > 2) {
            self->DialogShowInfo(fields[2]);
        }
        if (strcmp(fields[1], "dismiss") == 0) {
            self->DialogDismiss();
        }
    }

    return 0;
}

static int message_socket_listen_event(int fd, short revents, void *data)
{
    MessageSocket* ms = (MessageSocket*)data;
    MessageSocket* client = ms->Accept();
    printf("message_socket_listen_event: event on %d\n", fd);
    if (client) {
        printf("message_socket client connected\n");
        ev_add_fd(client->fd(), message_socket_client_event, client);
    }
    return 0;
}

RecoveryUI::RecoveryUI() :
    key_queue_len(0),
    key_last_down(-1),
    key_long_press(false),
    key_down_count(0),
    consecutive_power_keys(0),
    consecutive_alternate_keys(0),
    last_key(-1),
    in_touch(0),
    touch_x(0),
    touch_y(0),
    old_x(0),
    old_y(0),
    diff_x(0),
    diff_y(0),
    min_x_swipe_px(100),
    min_y_swipe_px(80),
    max_x_touch(0),
    max_y_touch(0),
    mt_count(0) {
    pthread_mutex_init(&key_queue_mutex, NULL);
    pthread_cond_init(&key_queue_cond, NULL);
    self = this;
}

void RecoveryUI::Init() {
    set_min_swipe_lengths();
    ev_init(input_callback, NULL);
    message_socket.ServerInit();
    ev_add_fd(message_socket.fd(), message_socket_listen_event, &message_socket);
    pthread_create(&input_t, NULL, input_thread, NULL);
}


int RecoveryUI::input_callback(int fd, short revents, void* data)
{
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    self->process_swipe(fd, &ev);

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            self->rel_sum += ev.value;
            if (self->rel_sum > 3) {
                self->process_key(KEY_DOWN, 1);   // press down key
                self->process_key(KEY_DOWN, 0);   // and release it
                self->rel_sum = 0;
            } else if (self->rel_sum < -3) {
                self->process_key(KEY_UP, 1);     // press up key
                self->process_key(KEY_UP, 0);     // and release it
                self->rel_sum = 0;
            }
        }
    } else {
        self->rel_sum = 0;
    }

    if (ev.type == EV_KEY && ev.code <= KEY_MAX)
        self->process_key(ev.code, ev.value);

    return 0;
}

// Process a key-up or -down event.  A key is "registered" when it is
// pressed and then released, with no other keypresses or releases in
// between.  Registered keys are passed to CheckKey() to see if it
// should trigger a visibility toggle, an immediate reboot, or be
// queued to be processed next time the foreground thread wants a key
// (eg, for the menu).
//
// We also keep track of which keys are currently down so that
// CheckKey can call IsKeyPressed to see what other keys are held when
// a key is registered.
//
// updown == 1 for key down events; 0 for key up events
void RecoveryUI::process_key(int key_code, int updown) {
    bool register_key = false;
    bool long_press = false;

    pthread_mutex_lock(&key_queue_mutex);
    key_pressed[key_code] = updown;
    if (updown) {
        ++key_down_count;
        key_last_down = key_code;
        key_long_press = false;
        pthread_t th;
        key_timer_t* info = new key_timer_t;
        info->ui = this;
        info->key_code = key_code;
        info->count = key_down_count;
        pthread_create(&th, NULL, &RecoveryUI::time_key_helper, info);
        pthread_detach(th);
    } else {
        if (key_last_down == key_code) {
            long_press = key_long_press;
            register_key = true;
        }
        key_last_down = -1;
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (register_key) {
        NextCheckKeyIsLong(long_press);
        switch (CheckKey(key_code)) {
          case RecoveryUI::IGNORE:
            break;

          case RecoveryUI::TOGGLE:
            ShowText(!IsTextVisible());
            break;

          case RecoveryUI::REBOOT:
            vold_unmount_all();
            android_reboot(ANDROID_RB_RESTART, 0, 0);
            break;

          case RecoveryUI::ENQUEUE:
            EnqueueKey(key_code);
            break;

          case RecoveryUI::MOUNT_SYSTEM:
#ifndef NO_RECOVERY_MOUNT
            ensure_path_mounted("/system");
            Print("Mounted /system.");
#endif
            break;
        }
    }
}

void* RecoveryUI::time_key_helper(void* cookie) {
    key_timer_t* info = (key_timer_t*) cookie;
    info->ui->time_key(info->key_code, info->count);
    delete info;
    return NULL;
}

void RecoveryUI::time_key(int key_code, int count) {
    usleep(750000);  // 750 ms == "long"
    bool long_press = false;
    pthread_mutex_lock(&key_queue_mutex);
    if (key_last_down == key_code && key_down_count == count) {
        long_press = key_long_press = true;
    }
    pthread_mutex_unlock(&key_queue_mutex);
    if (long_press) KeyLongPress(key_code);
}

void RecoveryUI::set_min_swipe_lengths() {
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.sf.lcd_density", value, "0");
    int screen_density = atoi(value);
    if(screen_density > 0) {
        min_x_swipe_px = (int)(0.5 * screen_density); // Roughly 0.5in
        min_y_swipe_px = (int)(0.3 * screen_density); // Roughly 0.3in
    }
}

void RecoveryUI::reset_gestures() {
    diff_x = 0;
    diff_y = 0;
    old_x = 0;
    old_y = 0;
    touch_x = 0;
    touch_y = 0;
}

void RecoveryUI::process_swipe(int fd, struct input_event *ev) {

    if (max_x_touch == 0 || max_y_touch == 0) {
        int abs_store[6] = {0};
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), abs_store);
        self->max_x_touch = abs_store[2];

        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), abs_store);
        self->max_y_touch = abs_store[2];
    }

    if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        if (ev->value == KEY_DOWN)
            mt_count++;
        else if (mt_count > 0 && ev->value == KEY_UP)
            mt_count--;

        if (mt_count == 0)
            reset_gestures();

    } else if (ev->type == EV_SYN) {
        //Print("x=%d y=%d dx=%d dy=%d\n", diff_x, diff_y, min_x_swipe_px, min_y_swipe_px);
        if (in_touch == 0 && ev->code == SYN_MT_REPORT) {
            reset_gestures();
            return;
        }
        in_touch = 0;
        if (diff_y > min_y_swipe_px) {
            EnqueueKey(KEY_VOLUMEDOWN);
            reset_gestures();
        } else if (diff_y < -min_y_swipe_px) {
            EnqueueKey(KEY_VOLUMEUP);
            reset_gestures();
        } else if (diff_x > min_x_swipe_px) {
            EnqueueKey(KEY_POWER);
            reset_gestures();
        } else if (diff_x < -min_x_swipe_px) {
            EnqueueKey(KEY_BACK);
            reset_gestures();
        }

    } else if (ev->type == EV_ABS && ev->code == ABS_MT_POSITION_X) {

        in_touch = 1;
        old_x = touch_x;
        float touch_x_rel = (float)ev->value / (float)self->max_x_touch;
        touch_x = touch_x_rel * gr_fb_width();

        if (old_x != 0)
            diff_x += touch_x - old_x;

    } else if (ev->type == EV_ABS && ev->code == ABS_MT_POSITION_Y) {

        in_touch = 1;
        old_y = touch_y;
        float touch_y_rel = (float)ev->value / (float)self->max_y_touch;
        touch_y = touch_y_rel * gr_fb_height();

        if (old_y != 0)
            diff_y += touch_y - old_y;
    }

    return;
}

void RecoveryUI::EnqueueKey(int key_code) {
    if (DialogShowing()) {
        if (DialogDismissable()) {
            DialogDismiss();
        }
        return;
    }
    pthread_mutex_lock(&key_queue_mutex);
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (key_queue_len < queue_max) {
        key_queue[key_queue_len++] = key_code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);
}


// Reads input events, handles special hot keys, and adds to the key queue.
void* RecoveryUI::input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void RecoveryUI::CancelWaitKey()
{
    pthread_mutex_lock(&key_queue_mutex);
    key_queue[key_queue_len] = -2;
    key_queue_len++;
    pthread_cond_signal(&key_queue_cond);
    pthread_mutex_unlock(&key_queue_mutex);
}

int RecoveryUI::WaitKey()
{
    pthread_mutex_lock(&key_queue_mutex);
    int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
    // plugged in.
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += 1;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
            if (VolumesChanged()) {
                pthread_mutex_unlock(&key_queue_mutex);
                return Device::kRefresh;
            }
        }
        timeouts--;
    } while ((timeouts || usb_connected()) && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

// Return true if USB is connected.
bool RecoveryUI::usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

bool RecoveryUI::IsKeyPressed(int key)
{
    pthread_mutex_lock(&key_queue_mutex);
    int pressed = key_pressed[key];
    pthread_mutex_unlock(&key_queue_mutex);
    return pressed;
}

void RecoveryUI::FlushKeys() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

// The default CheckKey implementation assumes the device has power,
// volume up, and volume down keys.
//
// - Hold power and press vol-up to toggle display.
// - Press power seven times in a row to reboot.
// - Alternate vol-up and vol-down seven times to mount /system.
RecoveryUI::KeyAction RecoveryUI::CheckKey(int key) {
    if (IsKeyPressed(KEY_POWER) && key == KEY_VOLUMEUP) {
        return TOGGLE;
    }

    if (key == KEY_POWER) {
        ++consecutive_power_keys;
        if (consecutive_power_keys >= 7) {
            return REBOOT;
        }
    } else {
        consecutive_power_keys = 0;
    }

    if ((key == KEY_VOLUMEUP &&
         (last_key == KEY_VOLUMEDOWN || last_key == -1)) ||
        (key == KEY_VOLUMEDOWN &&
         (last_key == KEY_VOLUMEUP || last_key == -1))) {
        ++consecutive_alternate_keys;
        if (consecutive_alternate_keys >= 7) {
            consecutive_alternate_keys = 0;
            return MOUNT_SYSTEM;
        }
    } else {
        consecutive_alternate_keys = 0;
    }
    last_key = key;

    return ENQUEUE;
}

void RecoveryUI::NextCheckKeyIsLong(bool is_long_press) {
}

void RecoveryUI::KeyLongPress(int key) {
}

void RecoveryUI::NotifyVolumesChanged() {
    v_changed = 1;
}

bool RecoveryUI::VolumesChanged() {
    int ret = v_changed;
    if (v_changed > 0)
        v_changed = 0;
    return ret == 1;
}
