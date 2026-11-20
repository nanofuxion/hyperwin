# Debugging a stalled adapter from inside the guest

These two programs are ours and use only public debugger-engine and system calls. The debugger itself
(Microsoft's WinDbg package) is never committed, never put in the app or its assets: fetch it into `gpu/work/dbg`
(ignored) and push it to the test guest by hand.

- `ld.c` makes a live kernel dump of the running system (`NtSystemDebugControl`, class 37). Run as SYSTEM.
- `lk.cpp` opens that dump with `dbgeng.dll` and runs the commands of a script, logging through `.logopen /u`
  (the log is UTF-16). `lk.exe <script> <log> <path to dbgeng.dll> <dump>`.

Local kernel debugging (`kd -kl`) did not work on this ARM64 guest (`AttachKernel` gave E_NOTIMPL), and the 2024 and later
`kd.exe` needs newer Windows than the guest has (entry point not found). The 2023 `dbgeng.dll` of the ARM64 WinDbg
package works. Symbols come from the public symbol server through the guest's network.
