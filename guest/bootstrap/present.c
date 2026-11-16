/* Bootstrap D3D11 present path. System d3d11.dll stays in place for anti-cheat.
 * A game that loads this copy publishes its clear color into the shared window.
 * Real drawing is the virtio-gpu gfxstream device, through Windows' own Vulkan loader.
 */
#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "map.h"
#include "ring.h"

#define DEVICE_SLOTS 43
#define CONTEXT_SLOTS 115
#define SWAP_SLOTS 18
#define SMALL_SLOTS 8

static void *device_vt[DEVICE_SLOTS];
static void *context_vt[CONTEXT_SLOTS];
static void *swap_vt[SWAP_SLOTS];
static void *tex_vt[SMALL_SLOTS];
static void *rtv_vt[SMALL_SLOTS];
static int ready;

static float clear_rgba[4];
static int have_clear;
static uint8_t frame[HW_WIDTH * HW_HEIGHT * 4];

struct com {
    void **vt;
    uint32_t refs;
};

static struct com device, context, swapchain, texture, rtv;

static HRESULT WINAPI notimpl(void)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI qi(struct com *self, void *iid, void **out)
{
    (void)iid;
    if (!out) return E_INVALIDARG;
    *out = self;
    self->refs++;
    return S_OK;
}

static uint32_t WINAPI addref(struct com *self) { return ++self->refs; }
static uint32_t WINAPI release(struct com *self) { return self->refs ? --self->refs : 0; }

static HRESULT WINAPI create_rtv(void *self, void *resource, const void *desc, void **view)
{
    (void)self;
    (void)resource;
    (void)desc;
    if (!view) return E_INVALIDARG;
    *view = &rtv;
    rtv.refs++;
    return S_OK;
}

static HRESULT WINAPI get_context(void *self, void **ctx)
{
    (void)self;
    if (!ctx) return E_INVALIDARG;
    *ctx = &context;
    context.refs++;
    return S_OK;
}

static HRESULT WINAPI clear_rtv(void *self, void *view, const float color[4])
{
    (void)self;
    (void)view;
    if (!color) return E_INVALIDARG;
    clear_rgba[0] = color[0];
    clear_rgba[1] = color[1];
    clear_rgba[2] = color[2];
    clear_rgba[3] = color[3];
    have_clear = 1;
    return S_OK;
}

static HRESULT WINAPI get_buffer(void *self, uint32_t index, void *iid, void **surface)
{
    (void)self;
    (void)iid;
    if (index != 0 || !surface) return E_INVALIDARG;
    *surface = &texture;
    texture.refs++;
    return S_OK;
}

static uint8_t chan(float c)
{
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (uint8_t)(c * 255.0f);
}

static HRESULT WINAPI present(void *self, uint32_t sync, uint32_t flags)
{
    struct hw_header *h = hw_guest_window();
    uint32_t n, i;
    uint8_t b = chan(clear_rgba[2]);
    uint8_t g = chan(clear_rgba[1]);
    uint8_t r = chan(clear_rgba[0]);
    uint8_t a = have_clear ? chan(clear_rgba[3]) : 255;
    (void)self;
    (void)sync;
    (void)flags;
    if (!have_clear) {
        b = 0x20;
        g = 0x30;
        r = 0x40;
    }
    n = HW_WIDTH * HW_HEIGHT;
    for (i = 0; i < n; i++) {
        frame[i * 4 + 0] = b;
        frame[i * 4 + 1] = g;
        frame[i * 4 + 2] = r;
        frame[i * 4 + 3] = a;
    }
    if (h) hw_publish_frame(h, frame, HW_WIDTH, HW_HEIGHT, HW_WIDTH * 4);
    return S_OK;
}

static void fill(void **vt, int n)
{
    int i;
    for (i = 0; i < n; i++) vt[i] = (void *)notimpl;
    vt[0] = (void *)qi;
    vt[1] = (void *)addref;
    vt[2] = (void *)release;
}

static void boot(void)
{
    if (ready) return;
    fill(device_vt, DEVICE_SLOTS);
    fill(context_vt, CONTEXT_SLOTS);
    fill(swap_vt, SWAP_SLOTS);
    fill(tex_vt, SMALL_SLOTS);
    fill(rtv_vt, SMALL_SLOTS);
    device_vt[9] = (void *)create_rtv;
    device_vt[40] = (void *)get_context;
    context_vt[50] = (void *)clear_rtv;
    swap_vt[8] = (void *)present;
    swap_vt[9] = (void *)get_buffer;
    device.vt = device_vt;
    context.vt = context_vt;
    swapchain.vt = swap_vt;
    texture.vt = tex_vt;
    rtv.vt = rtv_vt;
    device.refs = context.refs = swapchain.refs = texture.refs = rtv.refs = 1;
    ready = 1;
}

HRESULT WINAPI hw_create_device(const void *swap_desc, void **out_swap, void **out_device,
                                void **out_context)
{
    const uint32_t *wh = swap_desc;
    (void)wh;
    boot();
    if (out_swap) {
        *out_swap = &swapchain;
        swapchain.refs++;
    }
    if (out_device) {
        *out_device = &device;
        device.refs++;
    }
    if (out_context) {
        *out_context = &context;
        context.refs++;
    }
    return S_OK;
}
