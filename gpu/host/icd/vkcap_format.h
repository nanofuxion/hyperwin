// Binary trace format for triggered Vulkan capture (vkcap.cpp) and stand-alone off-screen
// replay (vkreplay.cpp). Captures the raw Vulkan calls that DXVK sends down to MaliVK/driver
// for a single named game, so a saved moment replays in seconds via adb shell with HyperWin closed.
#pragma once

#include <stdint.h>

namespace vkcap {

constexpr uint32_t kMagic = 0x50414356u;  // "VCAP"
constexpr uint32_t kVersion = 1u;

struct FileHeader {
    uint32_t magic;
    uint32_t version;
    char app_name[128];
};

struct RecHeader {
    uint32_t tag;
    uint32_t size;  // payload bytes following this header
};

enum RecTag : uint32_t {
    REC_DEVICE_INFO = 1,
    REC_ALLOC_MEM = 2,
    REC_MEM_DATA = 3,
    REC_CREATE_BUFFER = 4,
    REC_CREATE_IMAGE = 5,
    REC_BUFFER_UPLOAD = 6,
    REC_IMAGE_UPLOAD = 7,
    REC_CREATE_IMAGE_VIEW = 8,
    REC_CREATE_BUFFER_VIEW = 9,
    REC_CREATE_SAMPLER = 10,
    REC_CREATE_SHADER = 11,
    REC_CREATE_DSL = 12,
    REC_CREATE_PLAYOUT = 13,
    REC_CREATE_RENDERPASS = 14,
    REC_CREATE_FRAMEBUFFER = 15,
    REC_CREATE_GPIPE = 16,
    REC_CREATE_CPIPE = 17,
    REC_ALLOC_DSET = 18,
    REC_UPDATE_DSET = 19,
    REC_CREATE_QPOOL = 20,

    REC_SUBMIT_BEGIN = 100,
    REC_SUBMIT_END = 101,

    REC_CMD_BEGIN_RP = 200,
    REC_CMD_END_RP = 201,
    REC_CMD_NEXT_SUBPASS = 202,
    REC_CMD_BIND_PIPE = 203,
    REC_CMD_BIND_DSETS = 204,
    REC_CMD_BIND_VB = 205,
    REC_CMD_BIND_IB = 206,
    REC_CMD_PUSH_CONST = 207,
    REC_CMD_SET_VIEWPORT = 208,
    REC_CMD_SET_SCISSOR = 209,
    REC_CMD_SET_BLEND_CONST = 210,
    REC_CMD_SET_DEPTH_BIAS = 211,
    REC_CMD_SET_DEPTH_BOUNDS = 212,
    REC_CMD_SET_STENCIL_REF = 213,
    REC_CMD_SET_STENCIL_CMP = 214,
    REC_CMD_SET_STENCIL_WRITE = 215,
    REC_CMD_DRAW = 216,
    REC_CMD_DRAW_INDEXED = 217,
    REC_CMD_DRAW_INDIRECT = 218,
    REC_CMD_DRAW_INDEXED_INDIRECT = 219,
    REC_CMD_DISPATCH = 220,
    REC_CMD_CLEAR_ATT = 221,
    REC_CMD_CLEAR_COLOR = 222,
    REC_CMD_CLEAR_DS = 223,
    REC_CMD_COPY_BUF = 224,
    REC_CMD_COPY_BUF_TO_IMG = 225,
    REC_CMD_COPY_IMG = 226,
    REC_CMD_BLIT_IMG = 227,
    REC_CMD_RESOLVE_IMG = 228,
    REC_CMD_BARRIER = 229,
    REC_CMD_QUERY_RESET = 230,
    REC_CMD_QUERY_BEGIN = 231,
    REC_CMD_QUERY_END = 232,
    REC_CMD_QUERY_WRITE_TS = 233,
    REC_CMD_UPDATE_BUF = 234,
    REC_CMD_FILL_BUF = 235,

    REC_EOF = 999,
};

}  // namespace vkcap
