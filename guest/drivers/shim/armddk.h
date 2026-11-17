/* ARM64 pieces mingw-w64's wdm.h expects from a per-architecture header. */
#pragma once
#define PASSIVE_LEVEL 0
#define LOW_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define CLOCK_LEVEL 13
#define IPI_LEVEL 14
#define DRS_LEVEL 14
#define POWER_LEVEL 14
#define PROFILE_LEVEL 15
#define HIGH_LEVEL 15
#define PAGE_SIZE 0x1000
#define PAGE_SHIFT 12L
typedef struct _KFLOATING_SAVE { ULONG Dummy; } KFLOATING_SAVE, *PKFLOATING_SAVE;
NTHALAPI KIRQL NTAPI KeGetCurrentIrql(VOID);
NTHALAPI KIRQL NTAPI KfRaiseIrql(KIRQL NewIrql);
#define KeRaiseIrql(a, b) (*(b) = KfRaiseIrql(a))
#define KeFlushIoBuffers(_Mdl, _ReadOperation, _DmaOperation)
#define YieldProcessor() __asm__ volatile("yield")
NTKERNELAPI KIRQL NTAPI KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock);
NTKERNELAPI VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
#define KeAcquireSpinLock(a, b) (*(b) = KeAcquireSpinLockRaiseToDpc(a))
#ifndef InterlockedExchangePointer
#define InterlockedExchangePointer(t, v) __atomic_exchange_n((PVOID volatile *)(t), (PVOID)(v), __ATOMIC_SEQ_CST)
#endif
