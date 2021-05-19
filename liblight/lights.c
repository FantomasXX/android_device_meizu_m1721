/*
 * Copyright (C) 2014, 2017 The  Linux Foundation. All rights reserved.
 * Not a contribution
 * Copyright (C) 2008 The Android Open Source Project
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

// #define LOG_NDEBUG 0

#include <cutils/log.h>
#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include "lights_prv.h"
#include <hardware/lights.h>

#ifndef DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS
#define DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS 0x80
#endif

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static int g_last_backlight_mode = BRIGHTNESS_MODE_USER;
static int g_attention = 0;
static int g_brightness_max = 0;

char const *const LED_FILE = "/sys/class/leds/mx-led/brightness";

char const *const LCD_FILE = "/sys/class/leds/lcd-backlight/brightness";

char const *const LCD_FILE2 =
    "/sys/class/backlight/panel0-backlight/brightness";

char const *const LED_BLINK_FILE = "/sys/class/leds/mx-led/blink";

char const *const PERSISTENCE_FILE =
    "/sys/class/graphics/fb0/msm_fb_persist_mode";

/**
 * device methods
 */

void init_globals(void) {
  // init the mutex
  pthread_mutex_init(&g_lock, NULL);
}

static int write_int(char const *path, int value) {
  int fd;
  static int already_warned = 0;

  fd = open(path, O_RDWR);
  if (fd >= 0) {
    char buffer[20];
    int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
    ssize_t amt = write(fd, buffer, (size_t)bytes);
    close(fd);
    return amt == -1 ? -errno : 0;
  } else {
    if (already_warned == 0) {
      ALOGE("write_int failed to open %s\n", path);
      already_warned = 1;
    }
    return -errno;
  }
}

static int is_lit(struct light_state_t const *state) {
  return state->color & 0x00ffffff;
}

static int rgb_to_brightness(struct light_state_t const *state) {
  int color = state->color & 0x00ffffff;
  return ((77 * ((color >> 16) & 0x00ff)) + (150 * ((color >> 8) & 0x00ff)) +
          (29 * (color & 0x00ff))) >>
         8;
}

static int set_light_backlight(struct light_device_t *dev,
                               struct light_state_t const *state) {
  int err = 0;
  int brightness = rgb_to_brightness(state);
  unsigned int lpEnabled =
      state->brightnessMode == BRIGHTNESS_MODE_LOW_PERSISTENCE;
  if (!dev) {
    return -1;
  }

  pthread_mutex_lock(&g_lock);
  // Toggle low persistence mode state
  if ((g_last_backlight_mode != state->brightnessMode && lpEnabled) ||
      (!lpEnabled &&
       g_last_backlight_mode == BRIGHTNESS_MODE_LOW_PERSISTENCE)) {
    if ((err = write_int(PERSISTENCE_FILE, lpEnabled)) != 0) {
      ALOGE("%s: Failed to write to %s: %s\n", __FUNCTION__, PERSISTENCE_FILE,
            strerror(errno));
    }
    if (lpEnabled != 0) {
      brightness = DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS;
    }
  }

  g_last_backlight_mode = state->brightnessMode;

  if (!err) {
    if (!access(LCD_FILE, F_OK)) {
      err = write_int(LCD_FILE, brightness);
    } else {
      err = write_int(LCD_FILE2, brightness);
    }
  }

  pthread_mutex_unlock(&g_lock);
  return err;
}

static int set_light_backlight_ext(struct light_device_t *dev,
                                   struct light_state_t const *state) {
  int err = 0;

  if (!dev) {
    return -1;
  }

  int brightness = state->color & 0x00ffffff;
  pthread_mutex_lock(&g_lock);

  if (brightness >= 0 && brightness <= g_brightness_max) {
    set_brightness_ext_level(brightness);
  }

  pthread_mutex_unlock(&g_lock);

  return err;
}

static int set_speaker_light_locked(struct light_device_t *dev,
                                    struct light_state_t const *state) {
  int brightness;
  int blink;
  int onMS, offMS;
  if (!dev) {
    return -1;
  }

  switch (state->flashMode) {
  case LIGHT_FLASH_TIMED:
    onMS = state->flashOnMS;
    offMS = state->flashOffMS;
    break;
  case LIGHT_FLASH_NONE:
  default:
    onMS = 0;
    offMS = 0;
    break;
  }

  brightness = rgb_to_brightness(state);

  if (onMS > 0 && offMS > 0)
      blink = 1;
  else 
    blink = 0;

  if (blink) {
    if (write_int(LED_BLINK_FILE, blink))
        write_int(LED_FILE, 0);
  } else {
    write_int(LED_FILE, brightness);
  }

  return 0;
}

static void handle_speaker_battery_locked(struct light_device_t *dev) {
  if (is_lit(&g_battery)) {
    set_speaker_light_locked(dev, &g_battery);
  } else {
    set_speaker_light_locked(dev, &g_notification);
  }
}

static int set_light_battery(struct light_device_t *dev,
                             struct light_state_t const *state) {
  pthread_mutex_lock(&g_lock);
  g_battery = *state;
  handle_speaker_battery_locked(dev);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static int set_light_notifications(struct light_device_t *dev,
                                   struct light_state_t const *state) {
  pthread_mutex_lock(&g_lock);
  g_notification = *state;
  handle_speaker_battery_locked(dev);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static int set_light_attention(struct light_device_t *dev,
                               struct light_state_t const *state) {
  pthread_mutex_lock(&g_lock);
  if (state->flashMode == LIGHT_FLASH_HARDWARE) {
    g_attention = state->flashOnMS;
  } else if (state->flashMode == LIGHT_FLASH_NONE) {
    g_attention = 0;
  }
  handle_speaker_battery_locked(dev);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/** Close the lights device */
static int close_lights(struct light_device_t *dev) {
  if (dev) {
    free(dev);
  }
  return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t *module, char const *name,
                       struct hw_device_t **device) {
  int (*set_light)(struct light_device_t * dev,
                   struct light_state_t const *state);

  if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
    char property[PROPERTY_VALUE_MAX];
    property_get("persist.extend.brightness", property, "0");

    if (!(strncmp(property, "1", PROPERTY_VALUE_MAX)) ||
        !(strncmp(property, "true", PROPERTY_VALUE_MAX))) {
      property_get("persist.display.max_brightness", property, "255");
      g_brightness_max = atoi(property);
      set_brightness_ext_init();
      set_light = set_light_backlight_ext;
    } else
      set_light = set_light_backlight;
  } else if (0 == strcmp(LIGHT_ID_BATTERY, name))
    set_light = set_light_battery;
  else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
    set_light = set_light_notifications;
  else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
    set_light = set_light_attention;
  else
    return -EINVAL;

  pthread_once(&g_init, init_globals);

  struct light_device_t *dev = malloc(sizeof(struct light_device_t));

  if (!dev)
    return -ENOMEM;

  memset(dev, 0, sizeof(*dev));

  dev->common.tag = HARDWARE_DEVICE_TAG;
  dev->common.version = LIGHTS_DEVICE_API_VERSION_2_0;
  dev->common.module = (struct hw_module_t *)module;
  dev->common.close = (int (*)(struct hw_device_t *))close_lights;
  dev->set_light = set_light;

  *device = (struct hw_device_t *)dev;
  return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights Module",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};
