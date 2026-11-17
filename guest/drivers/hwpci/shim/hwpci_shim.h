/* Declarations the mingw-w64 DDK headers lack, all exported by the 22621 ARM64 ntoskrnl. */
#pragma once
#define POOL_FLAG_NON_PAGED 0x0000000000000040ULL
#define POOL_FLAG_PAGED 0x0000000000000100ULL
NTKERNELAPI PVOID NTAPI ExAllocatePool2(ULONG64 Flags, SIZE_T NumberOfBytes, ULONG Tag);
