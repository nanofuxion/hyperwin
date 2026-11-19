// Builds one graphics pipeline from a vertex and a fragment SPIR-V file on the phone's own Vulkan driver,
// to see whether the driver's shader compiler survives them (Mali's crashes on DXVK's fixed-function
// Direct3D 9 vertex shader, wddm/README.md). Prints PASS or the error; a crash shows as the shell's signal.
//   spvtest <driver .so> <vertex.spv> <fragment.spv>
// Used with spirv-reduce from the Mac to cut a crashing shader down.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define I(fn) auto fn = reinterpret_cast<PFN_##fn>(gipa(inst, #fn))
#define D(fn) auto fn = reinterpret_cast<PFN_##fn>(gdpa(dev, #fn))
#define CHECK(x)                             \
    do {                                     \
        VkResult r_ = (x);                   \
        if (r_ != VK_SUCCESS) {              \
            printf("FAIL %s: %d\n", #x, r_); \
            return 1;                        \
        }                                    \
    } while (0)

static std::vector<uint32_t> load(const char* path) {
    std::vector<uint32_t> v;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        v.resize(size_t(n) / 4);
        if (fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
        fclose(f);
    }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (lib) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
    if (lib && !gipa) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) {
        printf("FAIL cannot load: %s\n", dlerror());
        return 1;
    }
    auto vert = load(argv[2]), frag = load(argv[3]);
    if (vert.empty() || frag.empty()) {
        printf("FAIL cannot read the shaders\n");
        return 1;
    }
    I(vkCreateInstance);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "spvtest", 1, "DXVK", VK_MAKE_VERSION(2, 7, 0), VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    CHECK(vkCreateInstance(&ici, nullptr, &inst));
    I(vkEnumeratePhysicalDevices);
    I(vkCreateDevice);
    gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    I(vkGetPhysicalDeviceFeatures);
    I(vkGetPhysicalDeviceProperties);
    VkPhysicalDeviceFeatures feat{};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceFeatures(pd, &feat);
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("%s: shaderClipDistance %u, maxClipDistances %u, shaderCullDistance %u\n", props.deviceName, feat.shaderClipDistance,
           props.limits.maxClipDistances, feat.shaderCullDistance);
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci};
    dci.pEnabledFeatures = &feat;   // everything the driver has, as DXVK asks for what it needs
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));
    D(vkCreateRenderPass); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
    D(vkCreateGraphicsPipelines);

    VkAttachmentDescription att{0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref};
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub};
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, nullptr, &rp));

    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, vert.size() * 4, vert.data()};
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &smi, nullptr, &vs));
    smi.codeSize = frag.size() * 4;
    smi.pCode = frag.data();
    CHECK(vkCreateShaderModule(dev, &smi, nullptr, &fs));

    // DXVK's Direct3D 9 layout, roughly: two uniform buffers, a storage buffer, then samplers.
    std::vector<VkDescriptorSetLayoutBinding> binds;
    for (uint32_t b = 0; b < 24; b++)
        binds.push_back({b, b < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, uint32_t(binds.size()), binds.data()};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
    VkVertexInputBindingDescription bind{0, 16 * 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[16];
    for (uint32_t i = 0; i < 16; i++) attrs[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &bind, 16, attrs};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp{0, 0, 4, 4, 0, 1};
    VkRect2D sc{{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = layout;
    gp.renderPass = rp;
    VkPipeline pipe;
    printf("building\n");
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
    printf("%s %d\n", r == VK_SUCCESS ? "PASS" : "FAIL vkCreateGraphicsPipelines", r);
    return r == VK_SUCCESS ? 0 : 1;
}
