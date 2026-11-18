/* zink_test: opens Mesa's Zink on the phone's Vulkan driver through the Gallium interface, with no window
 * system, as the pipe host will (opengl/README.md, phase 1). Logs what the screen offers (the OpenGL level
 * it can reach), clears a render target and reads a pixel back. Run in adb shell:
 *   ZINK_VULKAN_LIB=libvulkan.so ./zink_test
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"

struct pipe_screen *zink_create_screen(struct sw_winsys *winsys, const struct pipe_screen_config *config);

/* A crash prints its frames as offsets in this program, to look up with llvm-symbolizer. */
static void on_crash(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = (ucontext_t *)uc;
    Dl_info info;
    dladdr((void *)on_crash, &info);
    uintptr_t base = (uintptr_t)info.dli_fbase;
    fprintf(stderr, "crash %d at %p, frames (offset in the program):", sig, si->si_addr);
    fprintf(stderr, " %lx", (unsigned long)(u->uc_mcontext.pc - base));
    uintptr_t *fp = (uintptr_t *)u->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp; i++) {
        fprintf(stderr, " %lx", (unsigned long)(fp[1] - base));
        uintptr_t *next = (uintptr_t *)fp[0];
        if (next <= fp) break;
        fp = next;
    }
    fprintf(stderr, "\n");
    _exit(139);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "creating the screen\n");
    struct pipe_screen *s = zink_create_screen(NULL, NULL);
    fprintf(stderr, "screen %p\n", (void *)s);
    if (!s) {
        printf("FAIL: no Zink screen\n");
        return 1;
    }
    printf("screen: %s, vendor %s, device %s\n", s->get_name(s), s->get_vendor(s), s->get_device_vendor(s));
    const struct pipe_caps *c = &s->caps;
    printf("GLSL %u (compatibility %u), compute %d, texture 2D %u, render targets %u, cube levels %u\n", c->glsl_feature_level,
           c->glsl_feature_level_compatibility, (int)c->compute, c->max_texture_2d_size, c->max_render_targets, c->max_texture_cube_levels);
    struct pipe_context *p = s->context_create(s, NULL, 0);
    if (!p) {
        printf("FAIL: no context\n");
        return 1;
    }
    struct pipe_resource t;
    memset(&t, 0, sizeof t);
    t.target = PIPE_TEXTURE_2D;
    t.format = PIPE_FORMAT_R8G8B8A8_UNORM;
    t.width0 = 64;
    t.height0 = 64;
    t.depth0 = 1;
    t.array_size = 1;
    t.bind = PIPE_BIND_RENDER_TARGET;
    struct pipe_resource *r = s->resource_create(s, &t);
    struct pipe_framebuffer_state fb;
    memset(&fb, 0, sizeof fb);
    fb.width = 64;
    fb.height = 64;
    fb.nr_cbufs = 1;
    fb.cbufs[0].texture = r;
    fb.cbufs[0].format = t.format;
    p->set_framebuffer_state(p, &fb);
    union pipe_color_union green = {.f = {0.f, 1.f, 0.f, 1.f}};
    p->clear(p, PIPE_CLEAR_COLOR0, 1u, 0, NULL, &green, 0.0, 0);
    struct pipe_fence_handle *fence = NULL;
    p->flush(p, &fence, 0);
    if (fence) s->fence_finish(s, p, fence, OS_TIMEOUT_INFINITE);
    struct pipe_box box = {32, 32, 0, 1, 1, 1};
    struct pipe_transfer *x = NULL;
    unsigned char *px = p->texture_map(p, r, 0, PIPE_MAP_READ, &box, &x);
    int ok = px && px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255;
    printf("pixel %02x %02x %02x %02x: %s\n", px ? px[0] : 0, px ? px[1] : 0, px ? px[2] : 0, px ? px[3] : 0, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
