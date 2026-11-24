// libvulkan_vkcap.so: Triggered Vulkan capture wrapper for HyperWin's wrap.cpp stack.
//
// Captures the raw Vulkan calls that DXVK sends down to MaliVK/driver for a single named game
// (filtered via Android prop `debug.hwgpu.vkcap`, e.g. `borderlands`). Other processes such as
// dwm.exe and WUDFHost.exe pass straight through untouched.
//
// Trigger capture at any moment during gameplay:
//   adb shell setprop debug.hwgpu.vkcap.trigger 1
// Optional submit count (default 16):
//   adb shell setprop debug.hwgpu.vkcap.submits 16
// Output file:
//   $HWGPU_VK_CACHE_DIR/vkcap_<trigger>.vkcap (inside dev.hyperwin.app/cache/shaders/)

#include "vkcap_format.h"
#include "wrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "vkcap", __VA_ARGS__)
#else
#define LOG(...) (fprintf(stderr, "vkcap: " __VA_ARGS__), fputc('\n', stderr))
#endif

namespace vkcap {
namespace {

static std::string get_prop(const char* name) {
#ifdef __ANDROID__
    char buf[PROP_VALUE_MAX] = "";
    __system_property_get(name, buf);
    return std::string(buf);
#else
    const char* v = getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static std::string get_thread_name() {
    char buf[32]{};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    return std::string(buf);
}

static bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    if (needle == "*") return true;
    std::string h = hay, n = needle;
    for (char& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

// ---- binary writer ----

struct Writer {
    std::vector<uint8_t> buf;
    void u32(uint32_t v) { put(&v, 4); }
    void i32(int32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void f32(float v) { put(&v, 4); }
    void put(const void* p, size_t n) {
        if (!n) return;
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void str(const char* s) {
        uint32_t n = s ? uint32_t(strlen(s)) : 0;
        u32(n);
        if (n) put(s, n);
    }
    template <class T> void pod(const T& v) { put(&v, sizeof(T)); }
};

static void emit_rec(FILE* fp, uint32_t tag, const void* data, uint32_t size) {
    if (!fp) return;
    RecHeader rh{tag, size};
    fwrite(&rh, sizeof(rh), 1, fp);
    if (size && data) fwrite(data, 1, size, fp);
}

static void emit_rec(FILE* fp, uint32_t tag, const Writer& w) {
    emit_rec(fp, tag, w.buf.data(), uint32_t(w.buf.size()));
}

// ---- tracked object records ----

struct MemRec {
    uint64_t id = 0;
    uint64_t size = 0;
    uint32_t mem_type_idx = 0;
    uint32_t mem_prop_flags = 0;
    bool host_visible = false;
    void* mapped = nullptr;
    uint64_t mapped_off = 0;
    uint64_t mapped_size = 0;
    std::vector<uint8_t> shadow;  // used only during active capture for dirty-page diffs
};

struct BufRec {
    uint64_t id = 0;
    VkBufferCreateInfo ci{};
    uint64_t mem_id = 0;
    uint64_t mem_off = 0;
};

struct ImgRec {
    uint64_t id = 0;
    VkImageCreateInfo ci{};
    uint64_t mem_id = 0;
    uint64_t mem_off = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct ImgViewRec {
    uint64_t id = 0;
    uint64_t img_id = 0;
    VkImageViewCreateInfo ci{};
};

struct BufViewRec {
    uint64_t id = 0;
    uint64_t buf_id = 0;
    VkBufferViewCreateInfo ci{};
};

struct SamplerRec {
    uint64_t id = 0;
    VkSamplerCreateInfo ci{};
};

struct ShaderRec {
    uint64_t id = 0;
    std::vector<uint8_t> code;
};

struct DslRec {
    uint64_t id = 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<std::vector<uint64_t>> imm_samplers;  // per binding
};

struct PlayoutRec {
    uint64_t id = 0;
    std::vector<uint64_t> set_ids;
    std::vector<VkPushConstantRange> push_constants;
};

struct RpRec {
    uint64_t id = 0;
    std::vector<VkAttachmentDescription> attachments;
    struct Sub {
        VkSubpassDescriptionFlags flags = 0;
        VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
        std::vector<VkAttachmentReference> input, color, resolve;
        bool has_depth = false;
        VkAttachmentReference depth{};
        std::vector<uint32_t> preserve;
    };
    std::vector<Sub> subpasses;
    std::vector<VkSubpassDependency> deps;
};

struct FbRec {
    uint64_t id = 0;
    uint64_t rp_id = 0;
    uint32_t width = 0, height = 0, layers = 0;
    std::vector<uint64_t> view_ids;
};

struct PipeRec {
    uint64_t id = 0;
    bool is_compute = false;
    std::vector<uint8_t> serialized;  // ready-to-emit payload for REC_CREATE_GPIPE / REC_CREATE_CPIPE
};

struct DsetWriteRec {
    uint32_t dst_binding = 0;
    uint32_t dst_array_elem = 0;
    uint32_t desc_type = 0;
    struct ImgInfo { uint64_t samp_id, view_id; uint32_t layout; };
    struct BufInfo { uint64_t buf_id; uint64_t offset, range; };
    std::vector<ImgInfo> imgs;
    std::vector<BufInfo> bufs;
    std::vector<uint64_t> texels;
};

struct DsetRec {
    uint64_t id = 0;
    uint64_t pool_h = 0;
    uint64_t dsl_id = 0;
    std::map<std::pair<uint32_t, uint32_t>, DsetWriteRec> writes;  // (binding, arrayElem) -> latest write
    bool dirty_for_cap = false;
};

struct QpoolRec {
    uint64_t id = 0;
    VkQueryPoolCreateInfo ci{};
};

struct ImgUploadKey {
    uint64_t img_id;
    uint32_t mip;
    uint32_t layer;
    bool operator<(const ImgUploadKey& o) const {
        if (img_id != o.img_id) return img_id < o.img_id;
        if (mip != o.mip) return mip < o.mip;
        return layer < o.layer;
    }
};

struct ImgUploadData {
    VkBufferImageCopy region{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    std::vector<uint8_t> bytes;
};

struct BufUploadData {
    uint64_t buf_id = 0;
    uint64_t dst_offset = 0;
    std::vector<uint8_t> bytes;
};

struct PendingCopyBuf {
    uint64_t src_buf_id, dst_buf_id;
    std::vector<VkBufferCopy> regions;
};

struct PendingCopyImg {
    uint64_t src_buf_id, dst_img_id;
    VkImageLayout dst_layout;
    std::vector<VkBufferImageCopy> regions;
};

struct CmdBufRec {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> cmds;  // (RecTag, payload)
    std::vector<PendingCopyBuf> copy_bufs;
    std::vector<PendingCopyImg> copy_imgs;
    std::vector<std::pair<uint64_t, VkImageLayout>> layout_transitions;
    uint32_t draw_count = 0;
    uint32_t rp_count = 0;
    void clear() {
        cmds.clear();
        copy_bufs.clear();
        copy_imgs.clear();
        layout_transitions.clear();
        draw_count = 0;
        rp_count = 0;
    }
};

constexpr size_t kMaxUploadCacheBytes = 192u * 1024u * 1024u;  // 192 MiB RAM cap for pre-trigger uploads

struct CapDevice {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkInstance inst = VK_NULL_HANDLE;
    std::string app_name;
    VkPhysicalDeviceMemoryProperties mem_props{};
    std::vector<std::string> enabled_exts;
    VkPhysicalDeviceFeatures enabled_features{};

    std::mutex mu;
    uint64_t next_id = 1;

    std::unordered_map<uint64_t, MemRec> mems;
    std::unordered_map<uint64_t, BufRec> bufs;
    std::unordered_map<uint64_t, ImgRec> imgs;
    std::unordered_map<uint64_t, ImgViewRec> img_views;
    std::unordered_map<uint64_t, BufViewRec> buf_views;
    std::unordered_map<uint64_t, SamplerRec> samplers;
    std::unordered_map<uint64_t, ShaderRec> shaders;            // keyed by monotonic id (never overwritten!)
    std::unordered_map<uint64_t, uint64_t> shader_handle_to_id; // current VkShaderModule handle -> id
    std::unordered_map<uint64_t, std::vector<VkDescriptorUpdateTemplateEntry>> desc_tmpls;
    std::unordered_map<uint64_t, DslRec> dsls;
    std::unordered_map<uint64_t, PlayoutRec> playouts;
    std::unordered_map<uint64_t, RpRec> rps;
    std::unordered_map<uint64_t, FbRec> fbs;
    std::unordered_map<uint64_t, PipeRec> pipes;
    std::unordered_map<uint64_t, DsetRec> dsets;
    std::unordered_map<uint64_t, QpoolRec> qpools;
    std::unordered_map<uint64_t, CmdBufRec> cmdbufs;

    // Pre-trigger upload cache for device-local textures & buffers (so BCn textures replay faithfully!)
    std::map<ImgUploadKey, ImgUploadData> img_uploads;
    std::unordered_map<uint64_t, std::vector<BufUploadData>> buf_uploads;
    size_t upload_cache_bytes = 0;

    // Active capture state
    FILE* cap_fp = nullptr;
    int cap_gen = 0;
    uint32_t cap_submits_left = 0;
    uint32_t cap_submit_idx = 0;
    uint32_t cap_total_draws = 0;
    uint32_t cap_total_rps = 0;
    std::string cap_path;
    uint64_t submit_counter = 0;

    template <class M> uint64_t id_of(const M& m, uint64_t h) const {
        if (!h) return 0;
        auto it = m.find(h);
        return it == m.end() ? 0 : it->second.id;
    }

    // Read bytes from a host-visible buffer if possible.
    bool read_host_buf(uint64_t buf_id, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) {
        const BufRec* br = nullptr;
        for (const auto& kv : bufs) {
            if (kv.second.id == buf_id) { br = &kv.second; break; }
        }
        if (!br || !br->mem_id) return false;
        MemRec* mr = nullptr;
        VkDeviceMemory vk_mem = VK_NULL_HANDLE;
        for (auto& kv : mems) {
            if (kv.second.id == br->mem_id) { mr = &kv.second; vk_mem = (VkDeviceMemory)kv.first; break; }
        }
        if (!mr || !mr->host_visible) return false;
        uint64_t abs_off = br->mem_off + offset;
        if (abs_off + size > mr->size) return false;
        out.resize(size_t(size));
        if (mr->mapped && abs_off >= mr->mapped_off && abs_off + size <= mr->mapped_off + mr->mapped_size) {
            memcpy(out.data(), static_cast<const uint8_t*>(mr->mapped) + (abs_off - mr->mapped_off), size_t(size));
            return true;
        }
        void* p = nullptr;
        if (NEXT_D(dev, vkMapMemory)(dev, vk_mem, abs_off, size, 0, &p) == VK_SUCCESS && p) {
            memcpy(out.data(), p, size_t(size));
            NEXT_D(dev, vkUnmapMemory)(dev, vk_mem);
            return true;
        }
        return false;
    }

    // ---- object emitters ----
    void emit_mem(const MemRec& m) {
        Writer w;
        w.u64(m.id); w.u64(m.size); w.u32(m.mem_prop_flags); w.u32(m.host_visible ? 1u : 0u);
        emit_rec(cap_fp, REC_ALLOC_MEM, w);
    }
    void emit_buf(const BufRec& b) {
        Writer w;
        w.u64(b.id); w.pod(b.ci); w.u64(b.mem_id); w.u64(b.mem_off);
        emit_rec(cap_fp, REC_CREATE_BUFFER, w);
    }
    void emit_img(const ImgRec& im) {
        Writer w;
        w.u64(im.id); w.pod(im.ci); w.u64(im.mem_id); w.u64(im.mem_off); w.u32(uint32_t(im.layout));
        emit_rec(cap_fp, REC_CREATE_IMAGE, w);
    }
    void emit_img_view(const ImgViewRec& v) {
        Writer w;
        w.u64(v.id); w.u64(v.img_id); w.pod(v.ci);
        emit_rec(cap_fp, REC_CREATE_IMAGE_VIEW, w);
    }
    void emit_buf_view(const BufViewRec& v) {
        Writer w;
        w.u64(v.id); w.u64(v.buf_id); w.pod(v.ci);
        emit_rec(cap_fp, REC_CREATE_BUFFER_VIEW, w);
    }
    void emit_sampler(const SamplerRec& s) {
        Writer w;
        w.u64(s.id); w.pod(s.ci);
        emit_rec(cap_fp, REC_CREATE_SAMPLER, w);
    }
    void emit_shader(const ShaderRec& sh) {
        Writer w;
        w.u64(sh.id); w.u32(uint32_t(sh.code.size())); w.put(sh.code.data(), sh.code.size());
        emit_rec(cap_fp, REC_CREATE_SHADER, w);
    }
    void emit_dsl(const DslRec& d) {
        Writer w;
        w.u64(d.id);
        w.u32(uint32_t(d.bindings.size()));
        for (size_t i = 0; i < d.bindings.size(); i++) {
            w.pod(d.bindings[i]);
            uint32_t ns = uint32_t(d.imm_samplers[i].size());
            w.u32(ns);
            for (uint64_t sid : d.imm_samplers[i]) w.u64(sid);
        }
        emit_rec(cap_fp, REC_CREATE_DSL, w);
    }
    void emit_playout(const PlayoutRec& pl) {
        Writer w;
        w.u64(pl.id);
        w.u32(uint32_t(pl.set_ids.size()));
        for (uint64_t sid : pl.set_ids) w.u64(sid);
        w.u32(uint32_t(pl.push_constants.size()));
        for (const auto& pc : pl.push_constants) w.pod(pc);
        emit_rec(cap_fp, REC_CREATE_PLAYOUT, w);
    }
    void emit_rp(const RpRec& rp) {
        Writer w;
        w.u64(rp.id);
        w.u32(uint32_t(rp.attachments.size()));
        for (const auto& a : rp.attachments) w.pod(a);
        w.u32(uint32_t(rp.subpasses.size()));
        for (const auto& s : rp.subpasses) {
            w.u32(s.flags); w.u32(uint32_t(s.bind_point));
            w.u32(uint32_t(s.input.size())); for (const auto& r : s.input) w.pod(r);
            w.u32(uint32_t(s.color.size())); for (const auto& r : s.color) w.pod(r);
            w.u32(uint32_t(s.resolve.size())); for (const auto& r : s.resolve) w.pod(r);
            w.u32(s.has_depth ? 1u : 0u);
            if (s.has_depth) w.pod(s.depth);
            w.u32(uint32_t(s.preserve.size())); for (uint32_t p : s.preserve) w.u32(p);
        }
        w.u32(uint32_t(rp.deps.size()));
        for (const auto& d : rp.deps) w.pod(d);
        emit_rec(cap_fp, REC_CREATE_RENDERPASS, w);
    }
    void emit_fb(const FbRec& fb) {
        Writer w;
        w.u64(fb.id); w.u64(fb.rp_id); w.u32(fb.width); w.u32(fb.height); w.u32(fb.layers);
        w.u32(uint32_t(fb.view_ids.size()));
        for (uint64_t vid : fb.view_ids) w.u64(vid);
        emit_rec(cap_fp, REC_CREATE_FRAMEBUFFER, w);
    }
    void emit_pipe(const PipeRec& p) {
        emit_rec(cap_fp, p.is_compute ? REC_CREATE_CPIPE : REC_CREATE_GPIPE, p.serialized.data(), uint32_t(p.serialized.size()));
    }
    void emit_dset_alloc(const DsetRec& ds) {
        Writer w;
        w.u64(ds.id); w.u64(ds.dsl_id);
        emit_rec(cap_fp, REC_ALLOC_DSET, w);
    }
    void emit_dset_writes(const DsetRec& ds) {
        if (ds.writes.empty()) return;
        Writer w;
        w.u64(ds.id);
        w.u32(uint32_t(ds.writes.size()));
        for (const auto& kv : ds.writes) {
            const auto& wr = kv.second;
            w.u32(wr.dst_binding); w.u32(wr.dst_array_elem); w.u32(wr.desc_type);
            w.u32(uint32_t(wr.imgs.size()));
            for (const auto& ii : wr.imgs) { w.u64(ii.samp_id); w.u64(ii.view_id); w.u32(ii.layout); }
            w.u32(uint32_t(wr.bufs.size()));
            for (const auto& bi : wr.bufs) { w.u64(bi.buf_id); w.u64(bi.offset); w.u64(bi.range); }
            w.u32(uint32_t(wr.texels.size()));
            for (uint64_t tv : wr.texels) w.u64(tv);
        }
        emit_rec(cap_fp, REC_UPDATE_DSET, w);
    }
    void emit_qpool(const QpoolRec& qp) {
        Writer w;
        w.u64(qp.id); w.pod(qp.ci);
        emit_rec(cap_fp, REC_CREATE_QPOOL, w);
    }

    void snapshot_host_mem(VkDeviceMemory vk_mem, MemRec& m, bool full) {
        if (!m.host_visible || m.size == 0 || m.size > (64ull << 20)) return;
        bool temp_map = false;
        const uint8_t* ptr = nullptr;
        if (m.mapped && m.mapped_off == 0 && m.mapped_size >= m.size) {
            ptr = static_cast<const uint8_t*>(m.mapped);
        } else if (!m.mapped) {
            void* p = nullptr;
            if (NEXT_D(dev, vkMapMemory)(dev, vk_mem, 0, m.size, 0, &p) == VK_SUCCESS && p) {
                ptr = static_cast<const uint8_t*>(p);
                temp_map = true;
            }
        }
        if (!ptr) return;
        if (full || m.shadow.size() != m.size) {
            m.shadow.resize(size_t(m.size));
            memcpy(m.shadow.data(), ptr, size_t(m.size));
            // Emit non-zero 4KB runs to keep the file compact
            const size_t kPage = 4096;
            size_t off = 0;
            while (off < m.size) {
                size_t end = std::min<size_t>(off + kPage, size_t(m.size));
                bool nz = false;
                for (size_t i = off; i < end; i++) { if (ptr[i]) { nz = true; break; } }
                if (!nz) { off = end; continue; }
                size_t run_start = off;
                off = end;
                while (off < m.size && (off - run_start) < (1u << 20)) {
                    size_t ne = std::min<size_t>(off + kPage, size_t(m.size));
                    bool nz2 = false;
                    for (size_t i = off; i < ne; i++) { if (ptr[i]) { nz2 = true; break; } }
                    if (!nz2) break;
                    off = ne;
                }
                Writer w;
                w.u64(m.id); w.u64(run_start); w.u64(off - run_start);
                w.put(ptr + run_start, off - run_start);
                emit_rec(cap_fp, REC_MEM_DATA, w);
            }
        } else {
            const size_t kPage = 4096;
            size_t off = 0;
            while (off < m.size) {
                size_t end = std::min<size_t>(off + kPage, size_t(m.size));
                if (memcmp(ptr + off, m.shadow.data() + off, end - off) == 0) { off = end; continue; }
                size_t run_start = off;
                off = end;
                while (off < m.size && (off - run_start) < (1u << 20)) {
                    size_t ne = std::min<size_t>(off + kPage, size_t(m.size));
                    if (memcmp(ptr + off, m.shadow.data() + off, ne - off) == 0) break;
                    off = ne;
                }
                memcpy(m.shadow.data() + run_start, ptr + run_start, off - run_start);
                Writer w;
                w.u64(m.id); w.u64(run_start); w.u64(off - run_start);
                w.put(ptr + run_start, off - run_start);
                emit_rec(cap_fp, REC_MEM_DATA, w);
            }
        }
        if (temp_map) NEXT_D(dev, vkUnmapMemory)(dev, vk_mem);
    }

    void begin_capture(int gen) {
        cap_gen = gen;
        std::string s_sub = get_prop("debug.hwgpu.vkcap.submits");
        int nsub = atoi(s_sub.c_str());
        cap_submits_left = uint32_t(nsub > 0 ? nsub : 16);
        cap_submit_idx = 0;
        cap_total_draws = 0;
        cap_total_rps = 0;

        const char* cdir = getenv("HWGPU_VK_CACHE_DIR");
        if (!cdir || !cdir[0]) cdir = getenv("XDG_CACHE_HOME");
        if (!cdir || !cdir[0]) cdir = "/data/local/tmp";
        cap_path = std::string(cdir) + "/vkcap_" + std::to_string(gen) + ".vkcap";
        cap_fp = fopen(cap_path.c_str(), "wb");
        if (!cap_fp) {
            LOG("FAILED to open capture file %s", cap_path.c_str());
            return;
        }
        LOG("BEGIN CAPTURE '%s' -> %s (submits=%u, live: mem=%zu buf=%zu img=%zu sh=%zu pipe=%zu ds=%zu uploads=%zu/%zu MB)",
            app_name.c_str(), cap_path.c_str(), cap_submits_left,
            mems.size(), bufs.size(), imgs.size(), shaders.size(), pipes.size(), dsets.size(),
            img_uploads.size(), upload_cache_bytes >> 20);

        NEXT_D(dev, vkDeviceWaitIdle)(dev);

        FileHeader fh{};
        fh.magic = kMagic;
        fh.version = kVersion;
        snprintf(fh.app_name, sizeof(fh.app_name), "%s", app_name.c_str());
        fwrite(&fh, sizeof(fh), 1, cap_fp);

        {
            Writer w;
            w.u32(uint32_t(enabled_exts.size()));
            for (const auto& e : enabled_exts) w.str(e.c_str());
            w.pod(enabled_features);
            emit_rec(cap_fp, REC_DEVICE_INFO, w);
        }

        for (auto& kv : mems) {
            emit_mem(kv.second);
            snapshot_host_mem((VkDeviceMemory)kv.first, kv.second, true);
        }
        for (const auto& kv : bufs) emit_buf(kv.second);
        for (const auto& kv : imgs) emit_img(kv.second);
        for (const auto& kv : buf_uploads) {
            for (const auto& u : kv.second) {
                Writer w;
                w.u64(u.buf_id); w.u64(u.dst_offset); w.u64(u.bytes.size());
                w.put(u.bytes.data(), u.bytes.size());
                emit_rec(cap_fp, REC_BUFFER_UPLOAD, w);
            }
        }
        for (const auto& kv : img_uploads) {
            Writer w;
            w.u64(kv.first.img_id); w.pod(kv.second.region); w.u32(uint32_t(kv.second.layout));
            w.u64(kv.second.bytes.size()); w.put(kv.second.bytes.data(), kv.second.bytes.size());
            emit_rec(cap_fp, REC_IMAGE_UPLOAD, w);
        }
        for (const auto& kv : img_views) emit_img_view(kv.second);
        for (const auto& kv : buf_views) emit_buf_view(kv.second);
        for (const auto& kv : samplers) emit_sampler(kv.second);
        for (const auto& kv : shaders) emit_shader(kv.second);
        for (const auto& kv : dsls) emit_dsl(kv.second);
        for (const auto& kv : playouts) emit_playout(kv.second);
        for (const auto& kv : rps) emit_rp(kv.second);
        for (const auto& kv : fbs) emit_fb(kv.second);
        for (const auto& kv : pipes) emit_pipe(kv.second);
        for (auto& kv : dsets) {
            emit_dset_alloc(kv.second);
            emit_dset_writes(kv.second);
            kv.second.dirty_for_cap = false;
        }
        for (const auto& kv : qpools) emit_qpool(kv.second);
        fflush(cap_fp);
    }

    void end_capture() {
        if (!cap_fp) return;
        emit_rec(cap_fp, REC_EOF, nullptr, 0);
        long sz = ftell(cap_fp);
        fclose(cap_fp);
        cap_fp = nullptr;
        for (auto& kv : mems) kv.second.shadow.clear();
        LOG("CAPTURE COMPLETE -> %s (%.1f MB, %u submits, %u renderpasses, %u draws)",
            cap_path.c_str(), double(sz) / (1024.0 * 1024.0), cap_submit_idx, cap_total_rps, cap_total_draws);
    }
};

std::mutex g_global_mu;
std::unordered_map<VkInstance, std::string> g_tracked_instances;
std::unordered_map<VkDevice, std::unique_ptr<CapDevice>> g_devices;

static CapDevice* dev_of(VkDevice d) {
    std::lock_guard<std::mutex> lk(g_global_mu);
    auto it = g_devices.find(d);
    return it == g_devices.end() ? nullptr : it->second.get();
}

static CapDevice* dev_of_cmd(VkCommandBuffer cb) {
    VkDevice d = wrap::device_of(cb);
    return d ? dev_of(d) : nullptr;
}

static CapDevice* dev_of_queue(VkQueue q) {
    VkDevice d = wrap::device_of(q);
    return d ? dev_of(d) : nullptr;
}

// ---- hooks: memory, buffers, images ----

VKAPI_ATTR VkResult VKAPI_CALL h_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                const VkAllocationCallbacks* alloc, VkDeviceMemory* mem) {
    VkResult r = NEXT_D(device, vkAllocateMemory)(device, info, alloc, mem);
    if (r != VK_SUCCESS || !mem || !*mem) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        MemRec mr;
        mr.id = cd->next_id++;
        mr.size = info->allocationSize;
        mr.mem_type_idx = info->memoryTypeIndex;
        if (mr.mem_type_idx < cd->mem_props.memoryTypeCount) {
            mr.mem_prop_flags = cd->mem_props.memoryTypes[mr.mem_type_idx].propertyFlags;
            mr.host_visible = (mr.mem_prop_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        }
        cd->mems[(uint64_t)*mem] = mr;
        if (cd->cap_fp) cd->emit_mem(mr);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_FreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->mems.erase((uint64_t)mem);
    }
    NEXT_D(device, vkFreeMemory)(device, mem, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_MapMemory(VkDevice device, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size,
                                           VkMemoryMapFlags flags, void** ppData) {
    VkResult r = NEXT_D(device, vkMapMemory)(device, mem, off, size, flags, ppData);
    if (r == VK_SUCCESS && ppData && *ppData) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            auto it = cd->mems.find((uint64_t)mem);
            if (it != cd->mems.end()) {
                it->second.mapped = *ppData;
                it->second.mapped_off = off;
                it->second.mapped_size = (size == VK_WHOLE_SIZE) ? (it->second.size - off) : size;
            }
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_UnmapMemory(VkDevice device, VkDeviceMemory mem) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        auto it = cd->mems.find((uint64_t)mem);
        if (it != cd->mems.end()) {
            it->second.mapped = nullptr;
            it->second.mapped_off = 0;
            it->second.mapped_size = 0;
        }
    }
    NEXT_D(device, vkUnmapMemory)(device, mem);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                              const VkAllocationCallbacks* alloc, VkBuffer* buf) {
    VkResult r = NEXT_D(device, vkCreateBuffer)(device, info, alloc, buf);
    if (r != VK_SUCCESS || !buf || !*buf) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        BufRec br;
        br.id = cd->next_id++;
        br.ci = *info;
        br.ci.pNext = nullptr;
        br.ci.pQueueFamilyIndices = nullptr;
        br.ci.queueFamilyIndexCount = 0;
        cd->bufs[(uint64_t)*buf] = br;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_BindBufferMemory(VkDevice device, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    VkResult r = NEXT_D(device, vkBindBufferMemory)(device, buf, mem, off);
    if (r == VK_SUCCESS) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            auto it = cd->bufs.find((uint64_t)buf);
            if (it != cd->bufs.end()) {
                it->second.mem_id = cd->id_of(cd->mems, (uint64_t)mem);
                it->second.mem_off = off;
                if (cd->cap_fp) cd->emit_buf(it->second);
            }
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos) {
    VkResult r = NEXT_D(device, vkBindBufferMemory2)(device, count, infos);
    if (r == VK_SUCCESS) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            for (uint32_t i = 0; i < count; i++) {
                auto it = cd->bufs.find((uint64_t)infos[i].buffer);
                if (it != cd->bufs.end()) {
                    it->second.mem_id = cd->id_of(cd->mems, (uint64_t)infos[i].memory);
                    it->second.mem_off = infos[i].memoryOffset;
                    if (cd->cap_fp) cd->emit_buf(it->second);
                }
            }
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyBuffer(VkDevice device, VkBuffer buf, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) {
            auto it = cd->bufs.find((uint64_t)buf);
            if (it != cd->bufs.end()) {
                auto uit = cd->buf_uploads.find(it->second.id);
                if (uit != cd->buf_uploads.end()) {
                    for (const auto& u : uit->second) cd->upload_cache_bytes -= u.bytes.size();
                    cd->buf_uploads.erase(uit);
                }
                cd->bufs.erase(it);
            }
        }
    }
    NEXT_D(device, vkDestroyBuffer)(device, buf, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateImage(VkDevice device, const VkImageCreateInfo* info,
                                             const VkAllocationCallbacks* alloc, VkImage* img) {
    VkResult r = NEXT_D(device, vkCreateImage)(device, info, alloc, img);
    if (r != VK_SUCCESS || !img || !*img) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        ImgRec ir;
        ir.id = cd->next_id++;
        ir.ci = *info;
        ir.ci.pNext = nullptr;
        ir.ci.pQueueFamilyIndices = nullptr;
        ir.ci.queueFamilyIndexCount = 0;
        ir.layout = info->initialLayout;
        cd->imgs[(uint64_t)*img] = ir;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_BindImageMemory(VkDevice device, VkImage img, VkDeviceMemory mem, VkDeviceSize off) {
    VkResult r = NEXT_D(device, vkBindImageMemory)(device, img, mem, off);
    if (r == VK_SUCCESS) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            auto it = cd->imgs.find((uint64_t)img);
            if (it != cd->imgs.end()) {
                it->second.mem_id = cd->id_of(cd->mems, (uint64_t)mem);
                it->second.mem_off = off;
                if (cd->cap_fp) cd->emit_img(it->second);
            }
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_BindImageMemory2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos) {
    VkResult r = NEXT_D(device, vkBindImageMemory2)(device, count, infos);
    if (r == VK_SUCCESS) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            for (uint32_t i = 0; i < count; i++) {
                auto it = cd->imgs.find((uint64_t)infos[i].image);
                if (it != cd->imgs.end()) {
                    it->second.mem_id = cd->id_of(cd->mems, (uint64_t)infos[i].memory);
                    it->second.mem_off = infos[i].memoryOffset;
                    if (cd->cap_fp) cd->emit_img(it->second);
                }
            }
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyImage(VkDevice device, VkImage img, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) {
            auto it = cd->imgs.find((uint64_t)img);
            if (it != cd->imgs.end()) {
                uint64_t iid = it->second.id;
                for (auto uit = cd->img_uploads.lower_bound({iid, 0, 0}); uit != cd->img_uploads.end() && uit->first.img_id == iid;) {
                    cd->upload_cache_bytes -= uit->second.bytes.size();
                    uit = cd->img_uploads.erase(uit);
                }
                cd->imgs.erase(it);
            }
        }
    }
    NEXT_D(device, vkDestroyImage)(device, img, alloc);
}

// ---- hooks: views, samplers, shaders, layouts, renderpasses, framebuffers, pipelines, descriptors ----

VKAPI_ATTR VkResult VKAPI_CALL h_CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkImageView* view) {
    VkResult r = NEXT_D(device, vkCreateImageView)(device, info, alloc, view);
    if (r != VK_SUCCESS || !view || !*view) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        ImgViewRec vr;
        vr.id = cd->next_id++;
        vr.img_id = cd->id_of(cd->imgs, (uint64_t)info->image);
        vr.ci = *info;
        vr.ci.pNext = nullptr;
        vr.ci.image = VK_NULL_HANDLE;
        cd->img_views[(uint64_t)*view] = vr;
        if (cd->cap_fp) cd->emit_img_view(vr);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyImageView(VkDevice device, VkImageView v, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) cd->img_views.erase((uint64_t)v);
    }
    NEXT_D(device, vkDestroyImageView)(device, v, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateBufferView(VkDevice device, const VkBufferViewCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc, VkBufferView* view) {
    VkResult r = NEXT_D(device, vkCreateBufferView)(device, info, alloc, view);
    if (r != VK_SUCCESS || !view || !*view) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        BufViewRec vr;
        vr.id = cd->next_id++;
        vr.buf_id = cd->id_of(cd->bufs, (uint64_t)info->buffer);
        vr.ci = *info;
        vr.ci.pNext = nullptr;
        vr.ci.buffer = VK_NULL_HANDLE;
        cd->buf_views[(uint64_t)*view] = vr;
        if (cd->cap_fp) cd->emit_buf_view(vr);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyBufferView(VkDevice device, VkBufferView v, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) cd->buf_views.erase((uint64_t)v);
    }
    NEXT_D(device, vkDestroyBufferView)(device, v, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateSampler(VkDevice device, const VkSamplerCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkSampler* samp) {
    VkResult r = NEXT_D(device, vkCreateSampler)(device, info, alloc, samp);
    if (r != VK_SUCCESS || !samp || !*samp) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        SamplerRec sr;
        sr.id = cd->next_id++;
        sr.ci = *info;
        sr.ci.pNext = nullptr;
        cd->samplers[(uint64_t)*samp] = sr;
        if (cd->cap_fp) cd->emit_sampler(sr);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroySampler(VkDevice device, VkSampler s, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) cd->samplers.erase((uint64_t)s);
    }
    NEXT_D(device, vkDestroySampler)(device, s, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* info,
                                                    const VkAllocationCallbacks* alloc, VkShaderModule* mod) {
    VkResult r = NEXT_D(device, vkCreateShaderModule)(device, info, alloc, mod);
    if (r != VK_SUCCESS || !mod || !*mod) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        ShaderRec sr;
        sr.id = cd->next_id++;
        sr.code.assign(reinterpret_cast<const uint8_t*>(info->pCode),
                       reinterpret_cast<const uint8_t*>(info->pCode) + info->codeSize);
        uint64_t sid = sr.id;
        cd->shaders[sid] = std::move(sr);
        cd->shader_handle_to_id[(uint64_t)*mod] = sid;
        if (cd->cap_fp) cd->emit_shader(cd->shaders[sid]);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks* alloc) {
    // Keep shaders in map? DXVK destroys VkShaderModule right after creating pipelines,
    // so we MUST keep shaders alive in our map for snapshot pipeline recreation!
    NEXT_D(device, vkDestroyShaderModule)(device, m, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                                                           const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* dsl) {
    VkResult r = NEXT_D(device, vkCreateDescriptorSetLayout)(device, info, alloc, dsl);
    if (r != VK_SUCCESS || !dsl || !*dsl) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        DslRec dr;
        dr.id = cd->next_id++;
        dr.bindings.resize(info->bindingCount);
        dr.imm_samplers.resize(info->bindingCount);
        for (uint32_t i = 0; i < info->bindingCount; i++) {
            dr.bindings[i] = info->pBindings[i];
            if (info->pBindings[i].pImmutableSamplers) {
                for (uint32_t j = 0; j < info->pBindings[i].descriptorCount; j++)
                    dr.imm_samplers[i].push_back(cd->id_of(cd->samplers, (uint64_t)info->pBindings[i].pImmutableSamplers[j]));
            }
            dr.bindings[i].pImmutableSamplers = nullptr;
        }
        cd->dsls[(uint64_t)*dsl] = std::move(dr);
        if (cd->cap_fp) cd->emit_dsl(cd->dsls[(uint64_t)*dsl]);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* info,
                                                      const VkAllocationCallbacks* alloc, VkPipelineLayout* pl) {
    VkResult r = NEXT_D(device, vkCreatePipelineLayout)(device, info, alloc, pl);
    if (r != VK_SUCCESS || !pl || !*pl) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        PlayoutRec pr;
        pr.id = cd->next_id++;
        for (uint32_t i = 0; i < info->setLayoutCount; i++)
            pr.set_ids.push_back(cd->id_of(cd->dsls, (uint64_t)info->pSetLayouts[i]));
        for (uint32_t i = 0; i < info->pushConstantRangeCount; i++)
            pr.push_constants.push_back(info->pPushConstantRanges[i]);
        cd->playouts[(uint64_t)*pl] = std::move(pr);
        if (cd->cap_fp) cd->emit_playout(cd->playouts[(uint64_t)*pl]);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc, VkRenderPass* rp) {
    VkResult r = NEXT_D(device, vkCreateRenderPass)(device, info, alloc, rp);
    if (r != VK_SUCCESS || !rp || !*rp) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        RpRec rr;
        rr.id = cd->next_id++;
        rr.attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
        rr.subpasses.resize(info->subpassCount);
        for (uint32_t i = 0; i < info->subpassCount; i++) {
            const auto& s = info->pSubpasses[i];
            auto& d = rr.subpasses[i];
            d.flags = s.flags;
            d.bind_point = s.pipelineBindPoint;
            if (s.pInputAttachments) d.input.assign(s.pInputAttachments, s.pInputAttachments + s.inputAttachmentCount);
            if (s.pColorAttachments) d.color.assign(s.pColorAttachments, s.pColorAttachments + s.colorAttachmentCount);
            if (s.pResolveAttachments) d.resolve.assign(s.pResolveAttachments, s.pResolveAttachments + s.colorAttachmentCount);
            if (s.pDepthStencilAttachment) { d.has_depth = true; d.depth = *s.pDepthStencilAttachment; }
            if (s.pPreserveAttachments) d.preserve.assign(s.pPreserveAttachments, s.pPreserveAttachments + s.preserveAttachmentCount);
        }
        if (info->pDependencies) rr.deps.assign(info->pDependencies, info->pDependencies + info->dependencyCount);
        cd->rps[(uint64_t)*rp] = std::move(rr);
        if (cd->cap_fp) cd->emit_rp(cd->rps[(uint64_t)*rp]);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                                   const VkAllocationCallbacks* alloc, VkFramebuffer* fb) {
    VkResult r = NEXT_D(device, vkCreateFramebuffer)(device, info, alloc, fb);
    if (r != VK_SUCCESS || !fb || !*fb) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        FbRec fr;
        fr.id = cd->next_id++;
        fr.rp_id = cd->id_of(cd->rps, (uint64_t)info->renderPass);
        fr.width = info->width; fr.height = info->height; fr.layers = info->layers;
        for (uint32_t i = 0; i < info->attachmentCount; i++)
            fr.view_ids.push_back(cd->id_of(cd->img_views, (uint64_t)info->pAttachments[i]));
        cd->fbs[(uint64_t)*fb] = std::move(fr);
        if (cd->cap_fp) cd->emit_fb(cd->fbs[(uint64_t)*fb]);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) cd->fbs.erase((uint64_t)fb);
    }
    NEXT_D(device, vkDestroyFramebuffer)(device, fb, alloc);
}

static void write_stage(Writer& w, CapDevice* cd, const VkPipelineShaderStageCreateInfo& st) {
    w.u32(st.flags);
    w.u32(uint32_t(st.stage));
    auto sit = cd->shader_handle_to_id.find((uint64_t)st.module);
    w.u64(sit == cd->shader_handle_to_id.end() ? 0 : sit->second);
    w.str(st.pName);
    if (st.pSpecializationInfo) {
        w.u32(1);
        w.u32(st.pSpecializationInfo->mapEntryCount);
        for (uint32_t j = 0; j < st.pSpecializationInfo->mapEntryCount; j++)
            w.pod(st.pSpecializationInfo->pMapEntries[j]);
        w.u32(uint32_t(st.pSpecializationInfo->dataSize));
        if (st.pSpecializationInfo->dataSize)
            w.put(st.pSpecializationInfo->pData, st.pSpecializationInfo->dataSize);
    } else {
        w.u32(0);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo* infos,
                                                         const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    VkResult r = NEXT_D(device, vkCreateGraphicsPipelines)(device, cache, count, infos, alloc, pipes);
    if (r != VK_SUCCESS) {
        if (auto* cd = dev_of(device)) {
            static std::atomic<uint32_t> nfail{0};
            uint32_t k = ++nfail;
            if (k <= 20 || (k % 50) == 0)
                LOG("FAIL vkCreateGraphicsPipelines #%u on '%s': r=%d count=%u stageCount=%u",
                    k, cd->app_name.c_str(), int(r), count, count ? infos[0].stageCount : 0u);
        }
        return r;
    }
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        for (uint32_t i = 0; i < count; i++) {
            if (!pipes[i]) continue;
            const auto& ci = infos[i];
            PipeRec pr;
            pr.id = cd->next_id++;
            pr.is_compute = false;
            Writer w;
            w.u64(pr.id);
            w.u32(ci.flags);
            w.u32(ci.stageCount);
            for (uint32_t j = 0; j < ci.stageCount; j++) write_stage(w, cd, ci.pStages[j]);
            // Vertex input
            w.u32(ci.pVertexInputState ? 1u : 0u);
            if (ci.pVertexInputState) {
                const auto* vi = ci.pVertexInputState;
                w.u32(vi->vertexBindingDescriptionCount);
                for (uint32_t j = 0; j < vi->vertexBindingDescriptionCount; j++) w.pod(vi->pVertexBindingDescriptions[j]);
                w.u32(vi->vertexAttributeDescriptionCount);
                for (uint32_t j = 0; j < vi->vertexAttributeDescriptionCount; j++) w.pod(vi->pVertexAttributeDescriptions[j]);
                // Optional vertex attribute divisor
                const VkPipelineVertexInputDivisorStateCreateInfoEXT* div = nullptr;
                for (auto* s = static_cast<const VkBaseInStructure*>(vi->pNext); s; s = s->pNext) {
                    if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                        div = reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT*>(s);
                        break;
                    }
                }
                w.u32(div ? div->vertexBindingDivisorCount : 0u);
                if (div) for (uint32_t j = 0; j < div->vertexBindingDivisorCount; j++) w.pod(div->pVertexBindingDivisors[j]);
            }
            // IA
            w.u32(ci.pInputAssemblyState ? 1u : 0u);
            if (ci.pInputAssemblyState) { VkPipelineInputAssemblyStateCreateInfo ia = *ci.pInputAssemblyState; ia.pNext = nullptr; w.pod(ia); }
            // Tess
            w.u32(ci.pTessellationState ? 1u : 0u);
            if (ci.pTessellationState) { VkPipelineTessellationStateCreateInfo ts = *ci.pTessellationState; ts.pNext = nullptr; w.pod(ts); }
            // Viewport
            w.u32(ci.pViewportState ? 1u : 0u);
            if (ci.pViewportState) {
                w.u32(ci.pViewportState->viewportCount);
                w.u32(ci.pViewportState->pViewports ? ci.pViewportState->viewportCount : 0u);
                if (ci.pViewportState->pViewports)
                    for (uint32_t j = 0; j < ci.pViewportState->viewportCount; j++) w.pod(ci.pViewportState->pViewports[j]);
                w.u32(ci.pViewportState->scissorCount);
                w.u32(ci.pViewportState->pScissors ? ci.pViewportState->scissorCount : 0u);
                if (ci.pViewportState->pScissors)
                    for (uint32_t j = 0; j < ci.pViewportState->scissorCount; j++) w.pod(ci.pViewportState->pScissors[j]);
            }
            // Raster
            w.u32(ci.pRasterizationState ? 1u : 0u);
            if (ci.pRasterizationState) { VkPipelineRasterizationStateCreateInfo rs = *ci.pRasterizationState; rs.pNext = nullptr; w.pod(rs); }
            // MSAA
            w.u32(ci.pMultisampleState ? 1u : 0u);
            if (ci.pMultisampleState) {
                VkPipelineMultisampleStateCreateInfo ms = *ci.pMultisampleState;
                uint32_t sm = ms.pSampleMask ? *ms.pSampleMask : 0xffffffffu;
                bool has_sm = ms.pSampleMask != nullptr;
                ms.pNext = nullptr; ms.pSampleMask = nullptr;
                w.pod(ms); w.u32(has_sm ? 1u : 0u); w.u32(sm);
            }
            // DS
            w.u32(ci.pDepthStencilState ? 1u : 0u);
            if (ci.pDepthStencilState) { VkPipelineDepthStencilStateCreateInfo ds = *ci.pDepthStencilState; ds.pNext = nullptr; w.pod(ds); }
            // ColorBlend
            w.u32(ci.pColorBlendState ? 1u : 0u);
            if (ci.pColorBlendState) {
                VkPipelineColorBlendStateCreateInfo cb = *ci.pColorBlendState;
                uint32_t ac = cb.attachmentCount;
                const auto* atts = cb.pAttachments;
                cb.pNext = nullptr; cb.pAttachments = nullptr;
                w.pod(cb);
                w.u32(ac);
                for (uint32_t j = 0; j < ac; j++) w.pod(atts[j]);
            }
            // Dynamic
            w.u32(ci.pDynamicState ? ci.pDynamicState->dynamicStateCount : 0u);
            if (ci.pDynamicState)
                for (uint32_t j = 0; j < ci.pDynamicState->dynamicStateCount; j++) w.u32(uint32_t(ci.pDynamicState->pDynamicStates[j]));
            w.u64(cd->id_of(cd->playouts, (uint64_t)ci.layout));
            w.u64(cd->id_of(cd->rps, (uint64_t)ci.renderPass));
            w.u32(ci.subpass);
            pr.serialized = std::move(w.buf);
            cd->pipes[(uint64_t)pipes[i]] = std::move(pr);
            if (cd->cap_fp) cd->emit_pipe(cd->pipes[(uint64_t)pipes[i]]);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                        const VkComputePipelineCreateInfo* infos,
                                                        const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    VkResult r = NEXT_D(device, vkCreateComputePipelines)(device, cache, count, infos, alloc, pipes);
    if (r != VK_SUCCESS) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        for (uint32_t i = 0; i < count; i++) {
            if (!pipes[i]) continue;
            PipeRec pr;
            pr.id = cd->next_id++;
            pr.is_compute = true;
            Writer w;
            w.u64(pr.id);
            w.u32(infos[i].flags);
            write_stage(w, cd, infos[i].stage);
            w.u64(cd->id_of(cd->playouts, (uint64_t)infos[i].layout));
            pr.serialized = std::move(w.buf);
            cd->pipes[(uint64_t)pipes[i]] = std::move(pr);
            if (cd->cap_fp) cd->emit_pipe(cd->pipes[(uint64_t)pipes[i]]);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info,
                                                        VkDescriptorSet* sets) {
    VkResult r = NEXT_D(device, vkAllocateDescriptorSets)(device, info, sets);
    if (r != VK_SUCCESS) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
            DsetRec dr;
            dr.id = cd->next_id++;
            dr.pool_h = (uint64_t)info->descriptorPool;
            dr.dsl_id = cd->id_of(cd->dsls, (uint64_t)info->pSetLayouts[i]);
            cd->dsets[(uint64_t)sets[i]] = std::move(dr);
            if (cd->cap_fp) cd->emit_dset_alloc(cd->dsets[(uint64_t)sets[i]]);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count,
                                                    const VkDescriptorSet* sets) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) for (uint32_t i = 0; i < count; i++) cd->dsets.erase((uint64_t)sets[i]);
    }
    return NEXT_D(device, vkFreeDescriptorSets)(device, pool, count, sets);
}

VKAPI_ATTR VkResult VKAPI_CALL h_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (!cd->cap_fp) {
            for (auto it = cd->dsets.begin(); it != cd->dsets.end();) {
                if (it->second.pool_h == (uint64_t)pool) it = cd->dsets.erase(it);
                else ++it;
            }
        }
    }
    return NEXT_D(device, vkResetDescriptorPool)(device, pool, flags);
}

VKAPI_ATTR void VKAPI_CALL h_UpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* writes,
                                                  uint32_t copyCount, const VkCopyDescriptorSet* copies) {
    NEXT_D(device, vkUpdateDescriptorSets)(device, writeCount, writes, copyCount, copies);
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        for (uint32_t i = 0; i < writeCount; i++) {
            const auto& wr = writes[i];
            auto it = cd->dsets.find((uint64_t)wr.dstSet);
            if (it == cd->dsets.end()) continue;
            DsetWriteRec rec;
            rec.dst_binding = wr.dstBinding;
            rec.dst_array_elem = wr.dstArrayElement;
            rec.desc_type = uint32_t(wr.descriptorType);
            switch (wr.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                if (wr.pImageInfo) {
                    for (uint32_t j = 0; j < wr.descriptorCount; j++) {
                        rec.imgs.push_back({cd->id_of(cd->samplers, (uint64_t)wr.pImageInfo[j].sampler),
                                            cd->id_of(cd->img_views, (uint64_t)wr.pImageInfo[j].imageView),
                                            uint32_t(wr.pImageInfo[j].imageLayout)});
                    }
                }
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                if (wr.pBufferInfo) {
                    for (uint32_t j = 0; j < wr.descriptorCount; j++) {
                        rec.bufs.push_back({cd->id_of(cd->bufs, (uint64_t)wr.pBufferInfo[j].buffer),
                                            wr.pBufferInfo[j].offset, wr.pBufferInfo[j].range});
                    }
                }
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                if (wr.pTexelBufferView) {
                    for (uint32_t j = 0; j < wr.descriptorCount; j++)
                        rec.texels.push_back(cd->id_of(cd->buf_views, (uint64_t)wr.pTexelBufferView[j]));
                }
                break;
            default: break;
            }
            it->second.writes[{wr.dstBinding, wr.dstArrayElement}] = std::move(rec);
            it->second.dirty_for_cap = true;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateDescriptorUpdateTemplate(VkDevice device,
                                                                const VkDescriptorUpdateTemplateCreateInfo* info,
                                                                const VkAllocationCallbacks* alloc,
                                                                VkDescriptorUpdateTemplate* tmpl) {
    VkResult r = NEXT_D(device, vkCreateDescriptorUpdateTemplate)(device, info, alloc, tmpl);
    if (r == VK_SUCCESS && tmpl && *tmpl && info) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            cd->desc_tmpls[(uint64_t)*tmpl].assign(info->pDescriptorUpdateEntries,
                                                   info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateDescriptorUpdateTemplateKHR(VkDevice device,
                                                                   const VkDescriptorUpdateTemplateCreateInfo* info,
                                                                   const VkAllocationCallbacks* alloc,
                                                                   VkDescriptorUpdateTemplate* tmpl) {
    VkResult r = NEXT_D(device, vkCreateDescriptorUpdateTemplateKHR)(device, info, alloc, tmpl);
    if (r == VK_SUCCESS && tmpl && *tmpl && info) {
        if (auto* cd = dev_of(device)) {
            std::lock_guard<std::mutex> lk(cd->mu);
            cd->desc_tmpls[(uint64_t)*tmpl].assign(info->pDescriptorUpdateEntries,
                                                   info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL h_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl,
                                                             const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->desc_tmpls.erase((uint64_t)tmpl);
    }
    NEXT_D(device, vkDestroyDescriptorUpdateTemplate)(device, tmpl, alloc);
}

VKAPI_ATTR void VKAPI_CALL h_DestroyDescriptorUpdateTemplateKHR(VkDevice device, VkDescriptorUpdateTemplate tmpl,
                                                                const VkAllocationCallbacks* alloc) {
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->desc_tmpls.erase((uint64_t)tmpl);
    }
    NEXT_D(device, vkDestroyDescriptorUpdateTemplateKHR)(device, tmpl, alloc);
}

static void record_template_update(CapDevice* cd, VkDescriptorSet dset, VkDescriptorUpdateTemplate tmpl, const void* data) {
    if (!data) return;
    std::lock_guard<std::mutex> lk(cd->mu);
    auto dit = cd->dsets.find((uint64_t)dset);
    auto tit = cd->desc_tmpls.find((uint64_t)tmpl);
    if (dit == cd->dsets.end() || tit == cd->desc_tmpls.end()) return;
    const auto* base = static_cast<const uint8_t*>(data);
    for (const auto& e : tit->second) {
        DsetWriteRec rec;
        rec.dst_binding = e.dstBinding;
        rec.dst_array_elem = e.dstArrayElement;
        rec.desc_type = uint32_t(e.descriptorType);
        switch (e.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            for (uint32_t j = 0; j < e.descriptorCount; j++) {
                const auto* ii = reinterpret_cast<const VkDescriptorImageInfo*>(base + e.offset + j * e.stride);
                rec.imgs.push_back({cd->id_of(cd->samplers, (uint64_t)ii->sampler),
                                    cd->id_of(cd->img_views, (uint64_t)ii->imageView),
                                    uint32_t(ii->imageLayout)});
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            for (uint32_t j = 0; j < e.descriptorCount; j++) {
                const auto* bi = reinterpret_cast<const VkDescriptorBufferInfo*>(base + e.offset + j * e.stride);
                rec.bufs.push_back({cd->id_of(cd->bufs, (uint64_t)bi->buffer), bi->offset, bi->range});
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            for (uint32_t j = 0; j < e.descriptorCount; j++) {
                const auto* bv = reinterpret_cast<const VkBufferView*>(base + e.offset + j * e.stride);
                rec.texels.push_back(cd->id_of(cd->buf_views, (uint64_t)*bv));
            }
            break;
        default: break;
        }
        dit->second.writes[{e.dstBinding, e.dstArrayElement}] = std::move(rec);
        dit->second.dirty_for_cap = true;
    }
}

VKAPI_ATTR void VKAPI_CALL h_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet dset,
                                                             VkDescriptorUpdateTemplate tmpl, const void* data) {
    NEXT_D(device, vkUpdateDescriptorSetWithTemplate)(device, dset, tmpl, data);
    if (auto* cd = dev_of(device)) record_template_update(cd, dset, tmpl, data);
}

VKAPI_ATTR void VKAPI_CALL h_UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet dset,
                                                                VkDescriptorUpdateTemplate tmpl, const void* data) {
    NEXT_D(device, vkUpdateDescriptorSetWithTemplateKHR)(device, dset, tmpl, data);
    if (auto* cd = dev_of(device)) record_template_update(cd, dset, tmpl, data);
}

VKAPI_ATTR VkResult VKAPI_CALL h_CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkQueryPool* qp) {
    VkResult r = NEXT_D(device, vkCreateQueryPool)(device, info, alloc, qp);
    if (r != VK_SUCCESS || !qp || !*qp) return r;
    if (auto* cd = dev_of(device)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        QpoolRec qr;
        qr.id = cd->next_id++;
        qr.ci = *info;
        qr.ci.pNext = nullptr;
        cd->qpools[(uint64_t)*qp] = qr;
        if (cd->cap_fp) cd->emit_qpool(qr);
    }
    return r;
}

// ---- hooks: command buffer recording ----

VKAPI_ATTR VkResult VKAPI_CALL h_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->cmdbufs[(uint64_t)cb].clear();
    }
    return NEXT_D(wrap::device_of(cb), vkBeginCommandBuffer)(cb, info);
}

VKAPI_ATTR VkResult VKAPI_CALL h_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->cmdbufs[(uint64_t)cb].clear();
    }
    return NEXT_D(wrap::device_of(cb), vkResetCommandBuffer)(cb, flags);
}

static void rec_cmd(CapDevice* cd, VkCommandBuffer cb, uint32_t tag, Writer&& w) {
    cd->cmdbufs[(uint64_t)cb].cmds.emplace_back(tag, std::move(w.buf));
}

VKAPI_ATTR void VKAPI_CALL h_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        auto& cr = cd->cmdbufs[(uint64_t)cb];
        cr.rp_count++;
        if (cd->cap_fp) {
            Writer w;
            w.u64(cd->id_of(cd->rps, (uint64_t)info->renderPass));
            w.u64(cd->id_of(cd->fbs, (uint64_t)info->framebuffer));
            w.pod(info->renderArea);
            w.u32(info->clearValueCount);
            for (uint32_t i = 0; i < info->clearValueCount; i++) w.pod(info->pClearValues[i]);
            w.u32(uint32_t(contents));
            rec_cmd(cd, cb, REC_CMD_BEGIN_RP, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBeginRenderPass)(cb, info, contents);
}

VKAPI_ATTR void VKAPI_CALL h_CmdEndRenderPass(VkCommandBuffer cb) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) rec_cmd(cd, cb, REC_CMD_END_RP, Writer{});
    }
    NEXT_D(wrap::device_of(cb), vkCmdEndRenderPass)(cb);
}

VKAPI_ATTR void VKAPI_CALL h_CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u32(uint32_t(contents)); rec_cmd(cd, cb, REC_CMD_NEXT_SUBPASS, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdNextSubpass)(cb, contents);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(uint32_t(bp)); w.u64(cd->id_of(cd->pipes, (uint64_t)p));
            rec_cmd(cd, cb, REC_CMD_BIND_PIPE, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindPipeline)(cb, bp, p);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                                   uint32_t firstSet, uint32_t count, const VkDescriptorSet* sets,
                                                   uint32_t dynCount, const uint32_t* dynOffsets) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w;
            w.u32(uint32_t(bp)); w.u64(cd->id_of(cd->playouts, (uint64_t)layout));
            w.u32(firstSet); w.u32(count);
            for (uint32_t i = 0; i < count; i++) w.u64(cd->id_of(cd->dsets, (uint64_t)sets[i]));
            w.u32(dynCount);
            for (uint32_t i = 0; i < dynCount; i++) w.u32(dynOffsets[i]);
            rec_cmd(cd, cb, REC_CMD_BIND_DSETS, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindDescriptorSets)(cb, bp, layout, firstSet, count, sets, dynCount, dynOffsets);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                  const VkBuffer* bufs, const VkDeviceSize* offsets) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(first); w.u32(count);
            for (uint32_t i = 0; i < count; i++) { w.u64(cd->id_of(cd->bufs, (uint64_t)bufs[i])); w.u64(offsets[i]); }
            rec_cmd(cd, cb, REC_CMD_BIND_VB, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindVertexBuffers)(cb, first, count, bufs, offsets);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                   const VkBuffer* bufs, const VkDeviceSize* offsets,
                                                   const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(first); w.u32(count);
            for (uint32_t i = 0; i < count; i++) { w.u64(cd->id_of(cd->bufs, (uint64_t)bufs[i])); w.u64(offsets[i]); }
            rec_cmd(cd, cb, REC_CMD_BIND_VB, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindVertexBuffers2)(cb, first, count, bufs, offsets, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindVertexBuffers2EXT(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                      const VkBuffer* bufs, const VkDeviceSize* offsets,
                                                      const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(first); w.u32(count);
            for (uint32_t i = 0; i < count; i++) { w.u64(cd->id_of(cd->bufs, (uint64_t)bufs[i])); w.u64(offsets[i]); }
            rec_cmd(cd, cb, REC_CMD_BIND_VB, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindVertexBuffers2EXT)(cb, first, count, bufs, offsets, sizes, strides);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize offset, VkIndexType type) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->bufs, (uint64_t)buf)); w.u64(offset); w.u32(uint32_t(type));
            rec_cmd(cd, cb, REC_CMD_BIND_IB, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBindIndexBuffer)(cb, buf, offset, type);
}

VKAPI_ATTR void VKAPI_CALL h_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                              uint32_t offset, uint32_t size, const void* values) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->playouts, (uint64_t)layout)); w.u32(stages); w.u32(offset); w.u32(size);
            w.put(values, size);
            rec_cmd(cd, cb, REC_CMD_PUSH_CONST, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdPushConstants)(cb, layout, stages, offset, size, values);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport* vps) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(first); w.u32(count);
            for (uint32_t i = 0; i < count; i++) w.pod(vps[i]);
            rec_cmd(cd, cb, REC_CMD_SET_VIEWPORT, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetViewport)(cb, first, count, vps);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D* scs) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(first); w.u32(count);
            for (uint32_t i = 0; i < count; i++) w.pod(scs[i]);
            rec_cmd(cd, cb, REC_CMD_SET_SCISSOR, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetScissor)(cb, first, count, scs);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.put(c, 16); rec_cmd(cd, cb, REC_CMD_SET_BLEND_CONST, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetBlendConstants)(cb, c);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetDepthBias(VkCommandBuffer cb, float cf, float clamp, float sf) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.f32(cf); w.f32(clamp); w.f32(sf); rec_cmd(cd, cb, REC_CMD_SET_DEPTH_BIAS, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetDepthBias)(cb, cf, clamp, sf);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetDepthBounds(VkCommandBuffer cb, float mn, float mx) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.f32(mn); w.f32(mx); rec_cmd(cd, cb, REC_CMD_SET_DEPTH_BOUNDS, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetDepthBounds)(cb, mn, mx);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t ref) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u32(f); w.u32(ref); rec_cmd(cd, cb, REC_CMD_SET_STENCIL_REF, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetStencilReference)(cb, f, ref);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u32(f); w.u32(m); rec_cmd(cd, cb, REC_CMD_SET_STENCIL_CMP, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetStencilCompareMask)(cb, f, m);
}

VKAPI_ATTR void VKAPI_CALL h_CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u32(f); w.u32(m); rec_cmd(cd, cb, REC_CMD_SET_STENCIL_WRITE, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdSetStencilWriteMask)(cb, f, m);
}

VKAPI_ATTR void VKAPI_CALL h_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->cmdbufs[(uint64_t)cb].draw_count++;
        if (cd->cap_fp) { Writer w; w.u32(vc); w.u32(ic); w.u32(fv); w.u32(fi); rec_cmd(cd, cb, REC_CMD_DRAW, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdDraw)(cb, vc, ic, fv, fi);
}

VKAPI_ATTR void VKAPI_CALL h_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inc, uint32_t fi, int32_t vo, uint32_t fins) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->cmdbufs[(uint64_t)cb].draw_count++;
        if (cd->cap_fp) { Writer w; w.u32(ic); w.u32(inc); w.u32(fi); w.i32(vo); w.u32(fins); rec_cmd(cd, cb, REC_CMD_DRAW_INDEXED, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdDrawIndexed)(cb, ic, inc, fi, vo, fins);
}

VKAPI_ATTR void VKAPI_CALL h_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u32(x); w.u32(y); w.u32(z); rec_cmd(cd, cb, REC_CMD_DISPATCH, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdDispatch)(cb, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL h_CmdClearAttachments(VkCommandBuffer cb, uint32_t ac, const VkClearAttachment* atts,
                                                 uint32_t rc, const VkClearRect* rects) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u32(ac); for (uint32_t i = 0; i < ac; i++) w.pod(atts[i]);
            w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(rects[i]);
            rec_cmd(cd, cb, REC_CMD_CLEAR_ATT, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdClearAttachments)(cb, ac, atts, rc, rects);
}

VKAPI_ATTR void VKAPI_CALL h_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout lay,
                                                const VkClearColorValue* col, uint32_t rc, const VkImageSubresourceRange* rng) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->imgs, (uint64_t)img)); w.u32(uint32_t(lay)); w.pod(*col);
            w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(rng[i]);
            rec_cmd(cd, cb, REC_CMD_CLEAR_COLOR, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdClearColorImage)(cb, img, lay, col, rc, rng);
}

VKAPI_ATTR void VKAPI_CALL h_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout lay,
                                                       const VkClearDepthStencilValue* ds, uint32_t rc, const VkImageSubresourceRange* rng) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->imgs, (uint64_t)img)); w.u32(uint32_t(lay)); w.pod(*ds);
            w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(rng[i]);
            rec_cmd(cd, cb, REC_CMD_CLEAR_DS, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdClearDepthStencilImage)(cb, img, lay, ds, rc, rng);
}

VKAPI_ATTR void VKAPI_CALL h_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t rc, const VkBufferCopy* regs) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        uint64_t sid = cd->id_of(cd->bufs, (uint64_t)src), did = cd->id_of(cd->bufs, (uint64_t)dst);
        cd->cmdbufs[(uint64_t)cb].copy_bufs.push_back({sid, did, std::vector<VkBufferCopy>(regs, regs + rc)});
        if (cd->cap_fp) {
            Writer w; w.u64(sid); w.u64(did); w.u32(rc);
            for (uint32_t i = 0; i < rc; i++) w.pod(regs[i]);
            rec_cmd(cd, cb, REC_CMD_COPY_BUF, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdCopyBuffer)(cb, src, dst, rc, regs);
}

VKAPI_ATTR void VKAPI_CALL h_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout lay,
                                                  uint32_t rc, const VkBufferImageCopy* regs) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        uint64_t sid = cd->id_of(cd->bufs, (uint64_t)src), did = cd->id_of(cd->imgs, (uint64_t)dst);
        cd->cmdbufs[(uint64_t)cb].copy_imgs.push_back({sid, did, lay, std::vector<VkBufferImageCopy>(regs, regs + rc)});
        if (cd->cap_fp) {
            Writer w; w.u64(sid); w.u64(did); w.u32(uint32_t(lay)); w.u32(rc);
            for (uint32_t i = 0; i < rc; i++) w.pod(regs[i]);
            rec_cmd(cd, cb, REC_CMD_COPY_BUF_TO_IMG, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdCopyBufferToImage)(cb, src, dst, lay, rc, regs);
}

VKAPI_ATTR void VKAPI_CALL h_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                          uint32_t rc, const VkImageCopy* regs) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->imgs, (uint64_t)src)); w.u32(uint32_t(sl));
            w.u64(cd->id_of(cd->imgs, (uint64_t)dst)); w.u32(uint32_t(dl));
            w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(regs[i]);
            rec_cmd(cd, cb, REC_CMD_COPY_IMG, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdCopyImage)(cb, src, sl, dst, dl, rc, regs);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                          uint32_t rc, const VkImageBlit* regs, VkFilter filter) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->imgs, (uint64_t)src)); w.u32(uint32_t(sl));
            w.u64(cd->id_of(cd->imgs, (uint64_t)dst)); w.u32(uint32_t(dl));
            w.u32(uint32_t(filter)); w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(regs[i]);
            rec_cmd(cd, cb, REC_CMD_BLIT_IMG, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBlitImage)(cb, src, sl, dst, dl, rc, regs, filter);
}

VKAPI_ATTR void VKAPI_CALL h_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                             uint32_t rc, const VkImageResolve* regs) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->imgs, (uint64_t)src)); w.u32(uint32_t(sl));
            w.u64(cd->id_of(cd->imgs, (uint64_t)dst)); w.u32(uint32_t(dl));
            w.u32(rc); for (uint32_t i = 0; i < rc; i++) w.pod(regs[i]);
            rec_cmd(cd, cb, REC_CMD_RESOLVE_IMG, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdResolveImage)(cb, src, sl, dst, dl, rc, regs);
}

VKAPI_ATTR void VKAPI_CALL h_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize sz, const void* data) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->bufs, (uint64_t)dst)); w.u64(off); w.u64(sz); w.put(data, size_t(sz));
            rec_cmd(cd, cb, REC_CMD_UPDATE_BUF, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdUpdateBuffer)(cb, dst, off, sz, data);
}

VKAPI_ATTR void VKAPI_CALL h_CmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize sz, uint32_t data) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) {
            Writer w; w.u64(cd->id_of(cd->bufs, (uint64_t)dst)); w.u64(off); w.u64(sz); w.u32(data);
            rec_cmd(cd, cb, REC_CMD_FILL_BUF, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdFillBuffer)(cb, dst, off, sz, data);
}

VKAPI_ATTR void VKAPI_CALL h_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                                VkDependencyFlags depFlags, uint32_t mc, const VkMemoryBarrier* mb,
                                                uint32_t bc, const VkBufferMemoryBarrier* bb,
                                                uint32_t ic, const VkImageMemoryBarrier* ib) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        auto& cr = cd->cmdbufs[(uint64_t)cb];
        for (uint32_t i = 0; i < ic; i++) {
            uint64_t iid = cd->id_of(cd->imgs, (uint64_t)ib[i].image);
            if (iid) cr.layout_transitions.push_back({iid, ib[i].newLayout});
        }
        if (cd->cap_fp) {
            Writer w;
            w.u32(srcStage); w.u32(dstStage); w.u32(depFlags);
            w.u32(mc); for (uint32_t i = 0; i < mc; i++) { VkMemoryBarrier m = mb[i]; m.pNext = nullptr; w.pod(m); }
            w.u32(bc);
            for (uint32_t i = 0; i < bc; i++) {
                VkBufferMemoryBarrier b = bb[i]; b.pNext = nullptr; b.buffer = VK_NULL_HANDLE;
                w.u64(cd->id_of(cd->bufs, (uint64_t)bb[i].buffer)); w.pod(b);
            }
            w.u32(ic);
            for (uint32_t i = 0; i < ic; i++) {
                VkImageMemoryBarrier b = ib[i]; b.pNext = nullptr; b.image = VK_NULL_HANDLE;
                w.u64(cd->id_of(cd->imgs, (uint64_t)ib[i].image)); w.pod(b);
            }
            rec_cmd(cd, cb, REC_CMD_BARRIER, std::move(w));
        }
    }
    NEXT_D(wrap::device_of(cb), vkCmdPipelineBarrier)(cb, srcStage, dstStage, depFlags, mc, mb, bc, bb, ic, ib);
}

VKAPI_ATTR void VKAPI_CALL h_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool qp, uint32_t first, uint32_t count) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u64(cd->id_of(cd->qpools, (uint64_t)qp)); w.u32(first); w.u32(count); rec_cmd(cd, cb, REC_CMD_QUERY_RESET, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdResetQueryPool)(cb, qp, first, count);
}

VKAPI_ATTR void VKAPI_CALL h_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q, VkQueryControlFlags f) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u64(cd->id_of(cd->qpools, (uint64_t)qp)); w.u32(q); w.u32(f); rec_cmd(cd, cb, REC_CMD_QUERY_BEGIN, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdBeginQuery)(cb, qp, q, f);
}

VKAPI_ATTR void VKAPI_CALL h_CmdEndQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q) {
    if (auto* cd = dev_of_cmd(cb)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        if (cd->cap_fp) { Writer w; w.u64(cd->id_of(cd->qpools, (uint64_t)qp)); w.u32(q); rec_cmd(cd, cb, REC_CMD_QUERY_END, std::move(w)); }
    }
    NEXT_D(wrap::device_of(cb), vkCmdEndQuery)(cb, qp, q);
}

// Compute tight byte size for a VkBufferImageCopy region (works for BC1-BC7 and uncompressed).
static uint64_t copy_region_bytes(VkFormat fmt, const VkBufferImageCopy& reg) {
    uint32_t bw = 1, bh = 1, bpb = 4;
    switch (fmt) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
        bw = 4; bh = 4; bpb = 8; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        bw = 4; bh = 4; bpb = 16; break;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
        bpb = 1; break;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_D16_UNORM:
        bpb = 2; break;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
        bpb = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        bpb = 16; break;
    default: bpb = 4; break;
    }
    uint32_t row_texels = reg.bufferRowLength ? reg.bufferRowLength : reg.imageExtent.width;
    uint32_t img_rows = reg.bufferImageHeight ? reg.bufferImageHeight : reg.imageExtent.height;
    uint64_t row_blocks = (row_texels + bw - 1) / bw;
    uint64_t col_blocks = (img_rows + bh - 1) / bh;
    uint64_t layers = std::max<uint32_t>(1u, reg.imageSubresource.layerCount);
    uint64_t depth = std::max<uint32_t>(1u, reg.imageExtent.depth);
    return row_blocks * col_blocks * bpb * depth * layers;
}

VKAPI_ATTR VkResult VKAPI_CALL h_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
    if (auto* cd = dev_of_queue(queue)) {
        std::lock_guard<std::mutex> lk(cd->mu);
        cd->submit_counter++;

        // 1. Check trigger before submitting (so this submit is the first recorded if armed!)
        if (!cd->cap_fp && (cd->submit_counter & 3u) == 0) {
            std::string trig = get_prop("debug.hwgpu.vkcap.trigger");
            int gen = atoi(trig.c_str());
            if (gen > 0 && gen != cd->cap_gen) {
                cd->begin_capture(gen);
            }
        }

        // 2. If NOT capturing, track host->device uploads & image layouts in RAM for future snapshot
        if (!cd->cap_fp) {
            for (uint32_t s = 0; s < submitCount; s++) {
                for (uint32_t c = 0; c < submits[s].commandBufferCount; c++) {
                    auto it = cd->cmdbufs.find((uint64_t)submits[s].pCommandBuffers[c]);
                    if (it == cd->cmdbufs.end()) continue;
                    const auto& cr = it->second;
                    for (const auto& lt : cr.layout_transitions) {
                        for (auto& ikv : cd->imgs) {
                            if (ikv.second.id == lt.first) { ikv.second.layout = lt.second; break; }
                        }
                    }
                    // Record device-local buffer uploads from host-visible staging buffers
                    if (cd->upload_cache_bytes < kMaxUploadCacheBytes) {
                        for (const auto& cb_copy : cr.copy_bufs) {
                            for (const auto& reg : cb_copy.regions) {
                                if (reg.size == 0 || reg.size > (16u << 20)) continue;
                                std::vector<uint8_t> bytes;
                                if (cd->read_host_buf(cb_copy.src_buf_id, reg.srcOffset, reg.size, bytes)) {
                                    auto& vec = cd->buf_uploads[cb_copy.dst_buf_id];
                                    // Replace if same offset & size
                                    bool replaced = false;
                                    for (auto& e : vec) {
                                        if (e.dst_offset == reg.dstOffset && e.bytes.size() == bytes.size()) {
                                            e.bytes = std::move(bytes);
                                            replaced = true;
                                            break;
                                        }
                                    }
                                    if (!replaced && cd->upload_cache_bytes + bytes.size() <= kMaxUploadCacheBytes) {
                                        cd->upload_cache_bytes += bytes.size();
                                        vec.push_back({cb_copy.dst_buf_id, reg.dstOffset, std::move(bytes)});
                                    }
                                }
                            }
                        }
                        for (const auto& ci_copy : cr.copy_imgs) {
                            VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
                            for (const auto& ikv : cd->imgs) {
                                if (ikv.second.id == ci_copy.dst_img_id) { fmt = ikv.second.ci.format; break; }
                            }
                            for (const auto& reg : ci_copy.regions) {
                                uint64_t sz = copy_region_bytes(fmt, reg);
                                if (sz == 0 || sz > (32u << 20)) continue;
                                std::vector<uint8_t> bytes;
                                if (cd->read_host_buf(ci_copy.src_buf_id, reg.bufferOffset, sz, bytes)) {
                                    ImgUploadKey key{ci_copy.dst_img_id, reg.imageSubresource.mipLevel, reg.imageSubresource.baseArrayLayer};
                                    auto oit = cd->img_uploads.find(key);
                                    if (oit != cd->img_uploads.end()) {
                                        cd->upload_cache_bytes -= oit->second.bytes.size();
                                    }
                                    if (cd->upload_cache_bytes + bytes.size() <= kMaxUploadCacheBytes) {
                                        cd->upload_cache_bytes += bytes.size();
                                        VkBufferImageCopy norm = reg;
                                        norm.bufferOffset = 0;
                                        cd->img_uploads[key] = {norm, ci_copy.dst_layout, std::move(bytes)};
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            // 3. ACTIVE CAPTURE: emit host-visible memory diffs + dirty descriptor sets + recorded commands!
            for (auto& kv : cd->mems) cd->snapshot_host_mem((VkDeviceMemory)kv.first, kv.second, false);
            for (auto& kv : cd->dsets) {
                if (kv.second.dirty_for_cap) {
                    cd->emit_dset_writes(kv.second);
                    kv.second.dirty_for_cap = false;
                }
            }
            Writer sb; sb.u32(cd->cap_submit_idx++);
            emit_rec(cd->cap_fp, REC_SUBMIT_BEGIN, sb);
            for (uint32_t s = 0; s < submitCount; s++) {
                for (uint32_t c = 0; c < submits[s].commandBufferCount; c++) {
                    auto it = cd->cmdbufs.find((uint64_t)submits[s].pCommandBuffers[c]);
                    if (it == cd->cmdbufs.end()) continue;
                    cd->cap_total_draws += it->second.draw_count;
                    cd->cap_total_rps += it->second.rp_count;
                    for (const auto& cmd : it->second.cmds) {
                        emit_rec(cd->cap_fp, cmd.first, cmd.second.data(), uint32_t(cmd.second.size()));
                    }
                }
            }
            emit_rec(cd->cap_fp, REC_SUBMIT_END, nullptr, 0);
            fflush(cd->cap_fp);
            if (cd->cap_submits_left > 0) cd->cap_submits_left--;
            if (cd->cap_submits_left == 0) cd->end_capture();
        }
    }
    return NEXT_D(wrap::device_of(queue), vkQueueSubmit)(queue, submitCount, submits, fence);
}

const wrap::Hook kHooks[] = {
    {"vkAllocateMemory", (PFN_vkVoidFunction)h_AllocateMemory},
    {"vkFreeMemory", (PFN_vkVoidFunction)h_FreeMemory},
    {"vkMapMemory", (PFN_vkVoidFunction)h_MapMemory},
    {"vkUnmapMemory", (PFN_vkVoidFunction)h_UnmapMemory},
    {"vkCreateBuffer", (PFN_vkVoidFunction)h_CreateBuffer},
    {"vkBindBufferMemory", (PFN_vkVoidFunction)h_BindBufferMemory},
    {"vkBindBufferMemory2", (PFN_vkVoidFunction)h_BindBufferMemory2},
    {"vkBindBufferMemory2KHR", (PFN_vkVoidFunction)h_BindBufferMemory2},
    {"vkDestroyBuffer", (PFN_vkVoidFunction)h_DestroyBuffer},
    {"vkCreateImage", (PFN_vkVoidFunction)h_CreateImage},
    {"vkBindImageMemory", (PFN_vkVoidFunction)h_BindImageMemory},
    {"vkBindImageMemory2", (PFN_vkVoidFunction)h_BindImageMemory2},
    {"vkBindImageMemory2KHR", (PFN_vkVoidFunction)h_BindImageMemory2},
    {"vkDestroyImage", (PFN_vkVoidFunction)h_DestroyImage},
    {"vkCreateImageView", (PFN_vkVoidFunction)h_CreateImageView},
    {"vkDestroyImageView", (PFN_vkVoidFunction)h_DestroyImageView},
    {"vkCreateBufferView", (PFN_vkVoidFunction)h_CreateBufferView},
    {"vkDestroyBufferView", (PFN_vkVoidFunction)h_DestroyBufferView},
    {"vkCreateSampler", (PFN_vkVoidFunction)h_CreateSampler},
    {"vkDestroySampler", (PFN_vkVoidFunction)h_DestroySampler},
    {"vkCreateShaderModule", (PFN_vkVoidFunction)h_CreateShaderModule},
    {"vkDestroyShaderModule", (PFN_vkVoidFunction)h_DestroyShaderModule},
    {"vkCreateDescriptorSetLayout", (PFN_vkVoidFunction)h_CreateDescriptorSetLayout},
    {"vkCreatePipelineLayout", (PFN_vkVoidFunction)h_CreatePipelineLayout},
    {"vkCreateRenderPass", (PFN_vkVoidFunction)h_CreateRenderPass},
    {"vkCreateFramebuffer", (PFN_vkVoidFunction)h_CreateFramebuffer},
    {"vkDestroyFramebuffer", (PFN_vkVoidFunction)h_DestroyFramebuffer},
    {"vkCreateGraphicsPipelines", (PFN_vkVoidFunction)h_CreateGraphicsPipelines},
    {"vkCreateComputePipelines", (PFN_vkVoidFunction)h_CreateComputePipelines},
    {"vkAllocateDescriptorSets", (PFN_vkVoidFunction)h_AllocateDescriptorSets},
    {"vkFreeDescriptorSets", (PFN_vkVoidFunction)h_FreeDescriptorSets},
    {"vkResetDescriptorPool", (PFN_vkVoidFunction)h_ResetDescriptorPool},
    {"vkUpdateDescriptorSets", (PFN_vkVoidFunction)h_UpdateDescriptorSets},
    {"vkCreateDescriptorUpdateTemplate", (PFN_vkVoidFunction)h_CreateDescriptorUpdateTemplate},
    {"vkCreateDescriptorUpdateTemplateKHR", (PFN_vkVoidFunction)h_CreateDescriptorUpdateTemplateKHR},
    {"vkDestroyDescriptorUpdateTemplate", (PFN_vkVoidFunction)h_DestroyDescriptorUpdateTemplate},
    {"vkDestroyDescriptorUpdateTemplateKHR", (PFN_vkVoidFunction)h_DestroyDescriptorUpdateTemplateKHR},
    {"vkUpdateDescriptorSetWithTemplate", (PFN_vkVoidFunction)h_UpdateDescriptorSetWithTemplate},
    {"vkUpdateDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)h_UpdateDescriptorSetWithTemplateKHR},
    {"vkCreateQueryPool", (PFN_vkVoidFunction)h_CreateQueryPool},
    {"vkBeginCommandBuffer", (PFN_vkVoidFunction)h_BeginCommandBuffer},
    {"vkResetCommandBuffer", (PFN_vkVoidFunction)h_ResetCommandBuffer},
    {"vkCmdBeginRenderPass", (PFN_vkVoidFunction)h_CmdBeginRenderPass},
    {"vkCmdEndRenderPass", (PFN_vkVoidFunction)h_CmdEndRenderPass},
    {"vkCmdNextSubpass", (PFN_vkVoidFunction)h_CmdNextSubpass},
    {"vkCmdBindPipeline", (PFN_vkVoidFunction)h_CmdBindPipeline},
    {"vkCmdBindDescriptorSets", (PFN_vkVoidFunction)h_CmdBindDescriptorSets},
    {"vkCmdBindVertexBuffers", (PFN_vkVoidFunction)h_CmdBindVertexBuffers},
    {"vkCmdBindVertexBuffers2", (PFN_vkVoidFunction)h_CmdBindVertexBuffers2},
    {"vkCmdBindVertexBuffers2EXT", (PFN_vkVoidFunction)h_CmdBindVertexBuffers2EXT},
    {"vkCmdBindIndexBuffer", (PFN_vkVoidFunction)h_CmdBindIndexBuffer},
    {"vkCmdPushConstants", (PFN_vkVoidFunction)h_CmdPushConstants},
    {"vkCmdSetViewport", (PFN_vkVoidFunction)h_CmdSetViewport},
    {"vkCmdSetScissor", (PFN_vkVoidFunction)h_CmdSetScissor},
    {"vkCmdSetBlendConstants", (PFN_vkVoidFunction)h_CmdSetBlendConstants},
    {"vkCmdSetDepthBias", (PFN_vkVoidFunction)h_CmdSetDepthBias},
    {"vkCmdSetDepthBounds", (PFN_vkVoidFunction)h_CmdSetDepthBounds},
    {"vkCmdSetStencilReference", (PFN_vkVoidFunction)h_CmdSetStencilReference},
    {"vkCmdSetStencilCompareMask", (PFN_vkVoidFunction)h_CmdSetStencilCompareMask},
    {"vkCmdSetStencilWriteMask", (PFN_vkVoidFunction)h_CmdSetStencilWriteMask},
    {"vkCmdDraw", (PFN_vkVoidFunction)h_CmdDraw},
    {"vkCmdDrawIndexed", (PFN_vkVoidFunction)h_CmdDrawIndexed},
    {"vkCmdDispatch", (PFN_vkVoidFunction)h_CmdDispatch},
    {"vkCmdClearAttachments", (PFN_vkVoidFunction)h_CmdClearAttachments},
    {"vkCmdClearColorImage", (PFN_vkVoidFunction)h_CmdClearColorImage},
    {"vkCmdClearDepthStencilImage", (PFN_vkVoidFunction)h_CmdClearDepthStencilImage},
    {"vkCmdCopyBuffer", (PFN_vkVoidFunction)h_CmdCopyBuffer},
    {"vkCmdCopyBufferToImage", (PFN_vkVoidFunction)h_CmdCopyBufferToImage},
    {"vkCmdCopyImage", (PFN_vkVoidFunction)h_CmdCopyImage},
    {"vkCmdBlitImage", (PFN_vkVoidFunction)h_CmdBlitImage},
    {"vkCmdResolveImage", (PFN_vkVoidFunction)h_CmdResolveImage},
    {"vkCmdUpdateBuffer", (PFN_vkVoidFunction)h_CmdUpdateBuffer},
    {"vkCmdFillBuffer", (PFN_vkVoidFunction)h_CmdFillBuffer},
    {"vkCmdPipelineBarrier", (PFN_vkVoidFunction)h_CmdPipelineBarrier},
    {"vkCmdResetQueryPool", (PFN_vkVoidFunction)h_CmdResetQueryPool},
    {"vkCmdBeginQuery", (PFN_vkVoidFunction)h_CmdBeginQuery},
    {"vkCmdEndQuery", (PFN_vkVoidFunction)h_CmdEndQuery},
    {"vkQueueSubmit", (PFN_vkVoidFunction)h_QueueSubmit},
    {nullptr, nullptr},
};

}  // namespace
}  // namespace vkcap

namespace wrap {

const Hook* hooks() { return vkcap::kHooks; }

void on_instance(VkInstance instance, const VkInstanceCreateInfo* info) {
    std::string filter = vkcap::get_prop("debug.hwgpu.vkcap");
    if (filter.empty()) return;
    std::string app = (info && info->pApplicationInfo && info->pApplicationInfo->pApplicationName)
                          ? info->pApplicationInfo->pApplicationName
                          : "";
    std::string tname = vkcap::get_thread_name();
    if (!vkcap::icontains(app, filter) && !vkcap::icontains(tname, filter)) {
        LOG("skipping VkInstance app='%s' thread='%s' (filter='%s')", app.c_str(), tname.c_str(), filter.c_str());
        return;
    }
    std::string label = !tname.empty() ? tname : app;
    std::lock_guard<std::mutex> lk(vkcap::g_global_mu);
    vkcap::g_tracked_instances[instance] = label;
    LOG("tracking Vulkan instance for '%s' (app='%s' thread='%s' filter='%s')",
        label.c_str(), app.c_str(), tname.c_str(), filter.c_str());
}

void on_device(VkPhysicalDevice pd, VkDevice device, const VkDeviceCreateInfo* info) {
    VkInstance inst = wrap::instance_of(pd);
    std::string app;
    {
        std::lock_guard<std::mutex> lk(vkcap::g_global_mu);
        auto it = vkcap::g_tracked_instances.find(inst);
        if (it != vkcap::g_tracked_instances.end()) {
            app = it->second;
        }
    }
    if (app.empty()) {
        std::string filter = vkcap::get_prop("debug.hwgpu.vkcap");
        std::string tname = vkcap::get_thread_name();
        if (!filter.empty() && vkcap::icontains(tname, filter)) {
            app = tname;
        } else {
            return;
        }
    }
    auto cd = std::make_unique<vkcap::CapDevice>();
    cd->dev = device;
    cd->pd = pd;
    cd->inst = inst;
    cd->app_name = app;
    NEXT_I(inst, vkGetPhysicalDeviceMemoryProperties)(pd, &cd->mem_props);
    if (info) {
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
            cd->enabled_exts.emplace_back(info->ppEnabledExtensionNames[i]);
        if (info->pEnabledFeatures) cd->enabled_features = *info->pEnabledFeatures;
    }
    LOG("tracking VkDevice %p for '%s' (%zu exts) — arm with: adb shell setprop debug.hwgpu.vkcap.trigger 1",
        (void*)device, app.c_str(), cd->enabled_exts.size());
    std::lock_guard<std::mutex> lk(vkcap::g_global_mu);
    vkcap::g_devices[device] = std::move(cd);
}

void on_device_destroy(VkDevice device) {
    std::unique_ptr<vkcap::CapDevice> dead;
    {
        std::lock_guard<std::mutex> lk(vkcap::g_global_mu);
        auto it = vkcap::g_devices.find(device);
        if (it != vkcap::g_devices.end()) {
            dead = std::move(it->second);
            vkcap::g_devices.erase(it);
        }
    }
    if (dead && dead->cap_fp) dead->end_capture();
}

}  // namespace wrap
