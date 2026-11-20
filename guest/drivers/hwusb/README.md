# hwusb: the virtual USB host controller (usb/README.md, phase 3)

Status 2026-11-20: a skeleton (`hwusb.c`) compiles against the local WDK headers (KMDF 1.31, UdeCx 1.0), not committed WDK
material. It is not built into a driver or shipped yet.

What the skeleton still needs from outside our sources: the framework binding that `wdfdriverentry.lib` and `udecxstub.lib`
normally provide. The undefined symbols are `WdfDriverGlobals`, `WdfFunctions_01031`, `WdfStructures`, `WdfStructureCount`,
`WdfClientVersionHigherThanFramework`, `UdecxDriverGlobals` and `UdecxFunctions`.

- KMDF: bindable by hand. Microsoft's open WDF source (gpu/work/Windows-Driver-Frameworks, MIT) shows what `WdfVersionBind`
  wants: a `WDF_BIND_INFO` with `FuncTable` set to the address of our `WdfFunctions_01031` pointer and `FuncCount` 458.
  `Parameters\Wdf\KmdfLibraryVersion` = 1.31 is injected like for viosock.
- UdeCx: a class extension. Its binding (`WdfVersionBindClass`, the class bind structure) is not in any public header or
  in the open source. Doing it by hand means reverse engineering `udecxstub.lib`.

Decision needed: link the two stub libraries into `hwusb.sys` (the standard, licensed use for a KMDF/UdeCx driver, but the
earlier rule says no WDK static libs), or reverse engineer the class binding by hand.
