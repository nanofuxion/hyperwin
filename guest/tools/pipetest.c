// Compiles many distinct Vulkan compute pipelines and times it, to see the phone-side shader cache
// work (shader-cache/README.md): a second run, after the renderer restarts, should be much faster.
//   pipetest.exe [pipelines] [first]   default 300 pipelines, specialisation values from 1
// Each pipeline is the same shader with another specialisation constant, so each one is new to the
// driver. No pipeline cache of its own: the wrapper on the phone adds one. Writes pipetest.txt
// beside the program.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static const unsigned int spirv[] = {
#include "pipe.inc"
};

#define GET(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int count = argc > 1 ? atoi(argv[1]) : 300, first = argc > 2 ? atoi(argv[2]) : 1;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "pipetest.txt");
    FILE *out = fopen(path, "a");
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { fprintf(out, "FAIL no vulkan-1.dll\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
    GET(NULL, vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pipetest", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(out, "FAIL vkCreateInstance\n"); return 1; }
    GET(inst, vkEnumeratePhysicalDevices);
    GET(inst, vkCreateDevice);
    GET(inst, vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(out, "FAIL vkCreateDevice\n"); return 1; }
#define DGET(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DGET(vkCreateShaderModule);
    DGET(vkCreateDescriptorSetLayout);
    DGET(vkCreatePipelineLayout);
    DGET(vkCreateComputePipelines);
    DGET(vkDestroyPipeline);
    DGET(vkDestroyDevice);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof spirv, spirv};
    VkShaderModule sm;
    vkCreateShaderModule(dev, &smci, NULL, &sm);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout set;
    vkCreateDescriptorSetLayout(dev, &dsl, NULL, &set);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set};
    VkPipelineLayout layout;
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double worst = 0, total = 0;
    for (int i = 0; i < count; i++) {
        uint32_t k = (uint32_t)(first + i);
        VkSpecializationMapEntry e = {0, 0, 4};
        VkSpecializationInfo si = {1, &e, 4, &k};
        VkComputePipelineCreateInfo c = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                          VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", &si},
                                         layout, VK_NULL_HANDLE, -1};
        VkPipeline p;
        QueryPerformanceCounter(&t0);
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &c, NULL, &p);
        QueryPerformanceCounter(&t1);
        if (r != VK_SUCCESS) { fprintf(out, "FAIL pipeline %d: %d\n", i, r); break; }
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        total += ms;
        if (ms > worst) worst = ms;
        vkDestroyPipeline(dev, p, NULL);
    }
    fprintf(out, "%d pipelines from %d: %.0f ms in all, %.2f ms each, worst %.1f ms\n", count, first, total,
            total / count, worst);
    vkDestroyDevice(dev, NULL);
    fclose(out);
    return 0;
}
