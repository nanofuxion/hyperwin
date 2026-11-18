/* Measures how fast a fullscreen Vulkan program can present: a borderless window over the whole
 * screen, a swapchain, and a clear per frame (a slow colour fade). Prints frames per second, so
 * the path frames take to the phone's screen can be compared (GDI copy or GPU scanout).
 *   vkpresent.exe [seconds] [immediate|mailbox|fifo] [windowed|srgb]   default: 5, immediate
 * Writes the result to stdout and to C:\HyperWin\vkpresent.txt.                                */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { report("FAIL %s: %d\n", #x, r_); return 1; } } while (0)

static FILE *out_file;

static void report(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (out_file) {
        va_start(a, fmt);
        vfprintf(out_file, fmt, a);
        va_end(a);
        fflush(out_file);
    }
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(w, m, wp, lp);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    out_file = fopen("C:\\HyperWin\\vkpresent.txt", "w");
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    VkPresentModeKHR want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (argc > 2 && !strcmp(argv[2], "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (argc > 2 && !strcmp(argv[2], "fifo")) want = VK_PRESENT_MODE_FIFO_KHR;
    int windowed = argc > 3 && !strcmp(argv[3], "windowed");
    VkFormat format = argc > 3 && !strcmp(argv[3], "srgb") ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { report("FAIL no vulkan-1.dll\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
#define I(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    VkInstance inst = VK_NULL_HANDLE;
    I(vkCreateInstance);
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkpresent", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext};
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    I(vkEnumeratePhysicalDevices); I(vkCreateDevice);
    I(vkGetDeviceProcAddr); I(vkCreateWin32SurfaceKHR); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    I(vkGetPhysicalDeviceSurfacePresentModesKHR);
    I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkGetPhysicalDeviceProperties);

    /* The window: the whole primary monitor, no border, in front. */
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0, wnd_proc, 0, 0, hi, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"vkpresent"};
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = windowed ? sw / 2 : sw, wh = windowed ? sh / 2 : sh;
    HWND hwnd = CreateWindowExW(windowed ? 0 : WS_EX_TOPMOST, L"vkpresent", L"vkpresent",
                                (windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP) | WS_VISIBLE, 0, 0, ww, wh, NULL, NULL, hi, NULL);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, hi, hwnd};
    VkSurfaceKHR surf;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));

    uint32_t n = 1;
    VkPhysicalDevice pd;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t qf = 0;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, qf, surf, &ok);
    if (!ok) { report("FAIL queue family 0 cannot present\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dext, NULL};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateSwapchainKHR); D(vkGetSwapchainImagesKHR); D(vkAcquireNextImageKHR);
    D(vkQueuePresentKHR); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdClearColorImage); D(vkQueueSubmit);
    D(vkCreateSemaphore); D(vkCreateFence); D(vkWaitForFences); D(vkResetFences); D(vkDeviceWaitIdle);
    D(vkResetCommandBuffer);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++) if (modes[i] == want) mode = want;
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)ww, (uint32_t)wh};
    uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, count,
                                   format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                   VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR chain;
    CHECK(vkCreateSwapchainKHR(dev, &sc, NULL, &chain));
    VkImage images[16];
    uint32_t ni = 16;
    CHECK(vkGetSwapchainImagesKHR(dev, chain, &ni, images));

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    enum { INFLIGHT = 2 };
    VkCommandBuffer cb[INFLIGHT];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, INFLIGHT};
    CHECK(vkAllocateCommandBuffers(dev, &cai, cb));
    VkSemaphore acquired[INFLIGHT], rendered[16];
    VkFence done[INFLIGHT];
    VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < INFLIGHT; i++) {
        CHECK(vkCreateSemaphore(dev, &semi, NULL, &acquired[i]));
        CHECK(vkCreateFence(dev, &fi, NULL, &done[i]));
    }
    for (uint32_t i = 0; i < ni; i++) CHECK(vkCreateSemaphore(dev, &semi, NULL, &rendered[i]));

    report("%s, %ux%u, %u images, mode %d%s%s\n", props.deviceName, ext.width, ext.height, ni, mode,
           windowed ? ", windowed" : "", format == VK_FORMAT_B8G8R8A8_SRGB ? ", sRGB" : "");
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    ULONGLONG tick0 = GetTickCount64();
    uint64_t frames = 0;
    double last_t = 0;
    VkResult last = VK_SUCCESS;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (getenv("VKP_TRACE") && (int)t != (int)last_t) {
            report("  %.1f s: %llu frames\n", t, (unsigned long long)frames);
        }
        last_t = t;
        if (t >= seconds) {
            report("%llu frames in %.2f s: %.1f FPS (last present %d; tick clock %.2f s)\n", (unsigned long long)frames, t,
                   frames / t, last, (GetTickCount64() - tick0) / 1000.0);
            break;
        }
        int f = (int)(frames % INFLIGHT);
        vkWaitForFences(dev, 1, &done[f], VK_TRUE, UINT64_MAX);
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, chain, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &idx);
        if (r < 0) { report("FAIL acquire %d after %llu frames\n", r, (unsigned long long)frames); break; }
        vkResetFences(dev, 1, &done[f]);
        vkResetCommandBuffer(cb[f], 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb[f], &bi);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier toClear = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[idx], range};
        vkCmdPipelineBarrier(cb[f], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                             0, NULL, 1, &toClear);
        float c = (float)(0.5 + 0.5 * sin(t * 1.5));
        VkClearColorValue color = {{0.1f, c * 0.6f, 0.9f - c * 0.6f, 1.0f}};
        vkCmdClearColorImage(cb[f], images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        VkImageMemoryBarrier toPresent = toClear;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb[f], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, 1, &toPresent);
        vkEndCommandBuffer(cb[f]);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired[f], &stage, 1, &cb[f], 1, &rendered[idx]};
        CHECK(vkQueueSubmit(q, 1, &si, done[f]));
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered[idx], 1, &chain, &idx, NULL};
        last = vkQueuePresentKHR(q, &pi);
        if (last < 0) { report("FAIL present %d after %llu frames\n", last, (unsigned long long)frames); break; }
        frames++;
    }
    vkDeviceWaitIdle(dev);
    DestroyWindow(hwnd);
    return 0;
}
