# Video decode passthrough

The phone's hardware video decoder (MediaCodec) serves the Windows guest.
Windows apps decode H.264, HEVC, VP9 and AV1 at hardware speed
without a real codec chip in the VM.

## How it works

```
Windows app -> Media Foundation -> hwvideo MFT (guest DLL)
                                      |  vsock 5660
Android app <- GuestVideo.kt ------> MediaCodec (Tensor HW decoder)
```

The guest MFT registers as a hardware decoder. When Edge, VLC, or any
Media Foundation app asks for H.264 or HEVC decode, Windows picks the MFT.
The MFT sends compressed NAL units to the Android app over vsock and gets
NV12 pictures back.

## Protocol

Reused from SweetCider (`sweetcider/tools/guest-ios/sweetvideo`).
Framed messages: a u32 LE header (type in the top byte, payload length in
the low 24 bits), then the payload.

### Guest to host

| Type | Name    | Payload                                                      |
|------|---------|--------------------------------------------------------------|
| 1    | open    | u32 session, u32 codec fourcc, u32 width, u32 height, config |
| 2    | frame   | u32 session, u64 tag, NAL data (length-prefixed units)       |
| 3    | flush   | u32 session                                                  |
| 4    | close   | u32 session                                                  |
| 5    | receipt | u32 session (flow control: one per picture received)         |

Codec fourcc: `avc1` for H.264, `hvc1` for HEVC. Config is the raw
avcC or hvcC box from the format description.

### Host to guest

| Type | Name    | Payload                                                           |
|------|---------|-------------------------------------------------------------------|
| 1    | opened  | u32 session, s32 status (0 = ok)                                  |
| 2    | picture | u32 session, u64 tag, s32 status, u32 width, u32 height, NV12    |
| 3    | flushed | u32 session                                                       |

NV12: Y plane tightly packed (width * height bytes), then interleaved
CbCr ((width+1)/2 * (height+1)/2 * 2 bytes).

## Pieces

### Host side (Android app)

Port SweetCider's `GuestVideo.kt`. Changes:

- Connect with `VirtualMachine.connectVsock(5660)` instead of
  SweetCider's `VmSmokeTest.connectGuest`.
- Add `HW_VSOCK_PORT_VIDEO = 5660` to `hw_abi.h`.
- Start/stop alongside the VM in `AvfLaunch`.

The `MediaCodecBackend` class, avcC/hvcC parsing, Annex B conversion,
and NV12 extraction are all reusable as-is.

### Guest side (Windows MFT)

A COM DLL that implements `IMFTransform` and registers via `MFTRegister`
with `MFT_ENUM_FLAG_HARDWARE`:

- **Input type:** H264 or HEVC compressed.
- **Output type:** NV12.
- **ProcessInput:** queues the compressed sample, sends a type-2 frame
  over the vsock link.
- **ProcessOutput:** blocks until a type-2 picture arrives, wraps the
  NV12 data in an `IMFSample`.
- **Drain/flush:** sends type-3, waits for the host's type-3 answer.

The vsock client uses the viosock driver (`\\?\Viosock`, same as
hwvsinput and hwagent). The MFT opens the socket on first
`ProcessInput` and reconnects if the link drops.

Build with the same cross-compiler chain as the other guest drivers
(`clang --target=aarch64-w64-mingw32`, `lld-link`). COM vtables are
plain C structs, no MSVC needed.

### Registration

`firstboot.cmd` or the guest agent copies `hwvideo.dll` to
`C:\HyperWin\` and runs:

```
regsvr32 /s C:\HyperWin\hwvideo.dll
```

Or the DLL's `DllRegisterServer` writes the MFT registry keys directly
(preferred, since it also sets the merit and supported media types).

## Bandwidth

| Resolution | NV12 frame | At 30 fps | At 60 fps |
|------------|-----------|-----------|-----------|
| 720p       | 1.4 MB    | 42 MB/s   | 83 MB/s   |
| 1080p      | 3.1 MB    | 93 MB/s   | 186 MB/s  |
| 4K         | 12.4 MB   | 374 MB/s  | 748 MB/s  |

Vsock on AVF handles 720p and 1080p fine. If 4K matters later, move the
decoded frames to a shared-memory ring (like the GPU ring) instead of
copying through vsock.

## What SweetCider proved

- Hardware decode on the phone is real: Tensor's codec handles H.264
  and HEVC at full speed, software fallback covers anything else.
- The protocol is simple and the latency is fine for video playback.
- Flow control (receipt messages) keeps the guest from flooding the host
  when the codec is slower than the source.
- The avcC/hvcC config and length-prefixed-to-Annex-B conversion are
  already debugged in `GuestVideo.kt`.
