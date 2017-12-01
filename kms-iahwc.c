/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/sysmacros.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/major.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <iahwc.h>
#include <libsync.h>

#include "common.h"

#define pfn_macro(name, capname)                                      \
  IAHWC_PFN_##capname iahwc_##name = (IAHWC_PFN_##capname)            \
    iahwc_device->getFunctionPtr(iahwc_device, IAHWC_FUNC_##capname);

iahwc_device_t* iahwc_device;
static const struct egl *egl;
static const struct gbm *gbm;

static int tty;

static void
reset_vt()
{
  struct vt_mode mode = { 0 };

  if (ioctl(tty, KDSETMODE, KD_TEXT))
    fprintf(stderr, "failed to set KD_TEXT mode on tty: %m\n");

  mode.mode = VT_AUTO;
  if (ioctl(tty, VT_SETMODE, &mode) < 0)
    fprintf(stderr, "could not reset vt handling\n");

  exit(0);
}


static void
handle_signal(int sig)
{
  reset_vt();
}

static int setup_tty() {
  struct vt_mode mode = {0};
  struct stat buf;
  int ret, kd_mode;

  tty = dup(STDIN_FILENO);

  if (fstat(tty, &buf) == -1 || major(buf.st_rdev) != TTY_MAJOR) {
    fprintf(stderr, "Please run the program in a vt \n");
    goto err_close;
  }

  ret = ioctl(tty, KDGETMODE, &kd_mode);
  if (ret) {
    fprintf(stderr, "failed to get VT mode: %m\n");
    return -1;
  }

  if (kd_mode != KD_TEXT) {
    fprintf(stderr,
            "Already in graphics mode, "
            "is a display server running?\n");
    goto err_close;
  }

  ioctl(tty, VT_ACTIVATE, minor(buf.st_rdev));
  ioctl(tty, VT_WAITACTIVE, minor(buf.st_rdev));

  ret = ioctl(tty, KDSETMODE, KD_GRAPHICS);
  if (ret) {
    fprintf(stderr, "failed to set KD_GRAPHICS mode on tty: %m\n");
    goto err_close;
  }

  mode.mode = VT_PROCESS;
  mode.relsig = 0;
  mode.acqsig = 0;
  if (ioctl(tty, VT_SETMODE, &mode) < 0) {
    fprintf(stderr, "failed to take control of vt handling\n");
    goto err_close;
  }

  struct sigaction act;
  act.sa_handler = handle_signal;
  act.sa_flags = SA_RESETHAND;

  sigaction(SIGINT, &act, NULL);
  sigaction(SIGSEGV, &act, NULL);
  sigaction(SIGABRT, &act, NULL);

  return 0;

err_close:
  close(tty);
  exit(0);
}

static EGLSyncKHR create_fence(const struct egl *egl, int fd)
{
  EGLint attrib_list[] = {
    EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
    EGL_NONE,
  };
  EGLSyncKHR fence = egl->eglCreateSyncKHR(egl->display,
                                           EGL_SYNC_NATIVE_FENCE_ANDROID, attrib_list);
  assert(fence);
  return fence;
}

static int run(const struct gbm *gbm, const struct egl *egl) {
  struct gbm_bo *bo = NULL;
  uint32_t i = 0;
  int ret;
  int kms_in_fence_fd;
  iahwc_layer_t layer;

  pfn_macro(create_layer, CREATE_LAYER);
  pfn_macro(present_display, PRESENT_DISPLAY);
  pfn_macro(layer_set_bo, LAYER_SET_BO);
  pfn_macro(layer_set_acquire_fence, LAYER_SET_ACQUIRE_FENCE);

  if (egl_check(egl, eglDupNativeFenceFDANDROID) ||
      egl_check(egl, eglCreateSyncKHR) ||
      egl_check(egl, eglDestroySyncKHR) ||
      egl_check(egl, eglWaitSyncKHR) ||
      egl_check(egl, eglClientWaitSyncKHR)) {
    printf("sync extensions not available\n");
    exit(EXIT_FAILURE);
  }

  ret = iahwc_create_layer(iahwc_device, 0, &layer);
  if (ret != IAHWC_ERROR_NONE) {
    printf("unable to create layer\n");
    exit(EXIT_FAILURE);
  }

  while (1) {
    struct gbm_bo *next_bo;
    EGLSyncKHR gpu_fence = NULL;
    int release_fence;

    printf("iteration: %d\n", i);
    fflush(stdout);
    egl->draw(i++);

    /* insert fence to be singled in cmdstream.. this fence will be
     * signaled when gpu rendering done
     */
    gpu_fence = create_fence(egl, EGL_NO_NATIVE_FENCE_FD_ANDROID);
    assert(gpu_fence);

    eglSwapBuffers(egl->display, egl->surface);

    /* after swapbuffers, gpu_fence should be flushed, so safe
     * to get fd:
     */

    kms_in_fence_fd = egl->eglDupNativeFenceFDANDROID(egl->display, gpu_fence);
    egl->eglDestroySyncKHR(egl->display, gpu_fence);
    assert(kms_in_fence_fd != -1);

    next_bo = gbm_surface_lock_front_buffer(gbm->surface);
    if (!next_bo) {
      printf("Failed to lock frontbuffer\n");
      return -1;
    }

    iahwc_layer_set_bo(iahwc_device, 0, layer, next_bo);
    iahwc_layer_set_acquire_fence(iahwc_device, 0, layer, kms_in_fence_fd);
    iahwc_present_display(iahwc_device, 0, &release_fence);

    printf("release fence is %d\n", release_fence);
    if (release_fence > 0) {
      ret = sync_wait(release_fence, -1);
      if (ret < 0)
        printf("failed to wait on fence %d: %s\n", release_fence, strerror(errno));
    }

    close(release_fence);

    /* release last buffer to render on again: */
    if (bo)
      gbm_surface_release_buffer(gbm->surface, bo);
    bo = next_bo;
  }
}

int vsync_callback (iahwc_callback_data_t data, iahwc_display_t display, int64_t timestamp) {

  static int64_t count = 0, base = -1;

  if (base == -1)
    base=timestamp;

  int64_t diff = (timestamp - base) / (1000 * 1000 * 1000);
  if (diff > 1) {
    printf("FPS: %ld\n", count);
    count = 0;
    base = timestamp;
  }
  count += 1;
  printf("timestamp for display %d and data %p is %ld\n", display, data, timestamp);
  return 0;

}

int main()
{
  freopen("log.txt", "a", stdout);
  freopen("log.txt", "a", stderr);

  const char* device = "/dev/dri/renderD128";
  void* dl_handle = dlopen("libhwcomposer.so", RTLD_NOW);
  setup_tty();

  iahwc_module_t* iahwc_module = dlsym(dl_handle, IAHWC_MODULE_STR);
  int num_displays;
  uint32_t name_size, configs_size, i;
  int mwidth=0, mheight=0;
  uint32_t* configs;
  uint32_t active_config;
  char* name;
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  int fd;

  iahwc_module->open(iahwc_module, &iahwc_device);
  pfn_macro(get_num_displays, GET_NUM_DISPLAYS);
  pfn_macro(get_display_name, GET_DISPLAY_NAME);
  pfn_macro(get_display_configs, GET_DISPLAY_CONFIGS);
  pfn_macro(get_display_info, GET_DISPLAY_INFO);
  pfn_macro(get_display_config, GET_DISPLAY_CONFIG);
  pfn_macro(register_callback, REGISTER_CALLBACK);

  iahwc_get_num_displays(iahwc_device, &num_displays);
  printf("The number of displays connected are %d\n", num_displays);

  iahwc_get_display_name(iahwc_device, 0, &name_size, NULL);
  printf("The length of the name %d\n", name_size);
  name = (char*) malloc((name_size + 1)*sizeof(char));
  name[name_size] = '\0';
  iahwc_get_display_name(iahwc_device, 0, &name_size, name);
  printf("Display name is %s\n", name);

  iahwc_get_display_configs(iahwc_device, 0, &configs_size, NULL);
  printf("The size of the configs %d\n", configs_size);
  configs = (uint32_t*) malloc(configs_size * sizeof(uint32_t));
  iahwc_get_display_configs(iahwc_device, 0, &configs_size, configs);

  for (i = 0; i < configs_size; i++) {
    int width, height, refresh_rate, dpix, dpiy;
    iahwc_get_display_info(iahwc_device, 0, configs[i], IAHWC_CONFIG_WIDTH, &width);
    iahwc_get_display_info(iahwc_device, 0, configs[i], IAHWC_CONFIG_HEIGHT, &height);
    iahwc_get_display_info(iahwc_device, 0, configs[i], IAHWC_CONFIG_REFRESHRATE, &refresh_rate);
    iahwc_get_display_info(iahwc_device, 0, configs[i], IAHWC_CONFIG_DPIX, &dpix);
    iahwc_get_display_info(iahwc_device, 0, configs[i], IAHWC_CONFIG_DPIY, &dpiy);

    printf("Config %d: width %d, height %d, refresh rate %d, dpix %d, dpiy %d\n", configs[i], width, height, refresh_rate, dpix, dpiy);

    if (!i) {
      mwidth = width;
      mheight = height;
    }

  }
  iahwc_get_display_config(iahwc_device, 0, &active_config);
  printf("Currently active config is %d\n", active_config);

  fd = open(device, O_RDWR);

  if (fd < 0) {
    printf("unable to open gpu file\n");
    exit(EXIT_FAILURE);
  }

  gbm = init_gbm(fd, mwidth, mheight, modifier);
  if (!gbm) {
    printf("failed to initialize GBM\n");
    exit(EXIT_FAILURE);
  }

  egl = init_cube_smooth(gbm);

  if (!egl) {
    printf("failed to initialize EGL\n");
    exit(EXIT_FAILURE);
  }

  /* clear the color buffer */
  glClearColor(0.5, 0.5, 0.5, 1.0);
  glClear(GL_COLOR_BUFFER_BIT);

  int ret = iahwc_register_callback(iahwc_device,
                                    IAHWC_CALLBACK_VSYNC,
                                    0,
                                    NULL,
                                    (iahwc_function_ptr_t)vsync_callback);

  if (ret != IAHWC_ERROR_NONE) {
    printf("unable to register vsync callback\n");
    return -1;
  }

  run(gbm, egl);


  printf("\n\n\n");
  fprintf(stderr, "\n");
  return 0;

}
