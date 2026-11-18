/* Bridges for building a UMDF 2 driver with mingw headers (gpu/guest/idd/build.sh). */
#define _Enum_is_bitflag_
#define __analysis_noreturn
#define CFORCEINLINE __forceinline
#define __control_entrypoint(x)
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
static __inline PVOID ReadPointerNoFence(PVOID const volatile *p) { return *p; }
static __inline UCHAR ReadUCharNoFence(UCHAR const volatile *p) { return *p; }
static __inline USHORT ReadUShortNoFence(USHORT const volatile *p) { return *p; }
static __inline ULONG ReadULongNoFence(ULONG const volatile *p) { return *p; }
static __inline ULONG64 ReadULong64NoFence(ULONG64 const volatile *p) { return *p; }
static __inline void WriteUCharNoFence(UCHAR volatile *p, UCHAR v) { *p = v; }
static __inline void WriteUShortNoFence(USHORT volatile *p, USHORT v) { *p = v; }
static __inline void WriteULongNoFence(ULONG volatile *p, ULONG v) { *p = v; }
static __inline void WriteULong64NoFence(ULONG64 volatile *p, ULONG64 v) { *p = v; }
#define _DataSynchronizationBarrier() __sync_synchronize()
