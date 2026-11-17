# hwpci ACPI contract

`hwpci.sys` binds to one ACPI device. The loader builds it from the live FDT node
`compatible = "pci-host-cam-generic"`. `hwpci.asl` is a compiling example.

## The node

| Object | Value |
| --- | --- |
| `_HID` | `"HWPC0001"` |
| `_UID` | `0` |
| `_CCA` | `One`. Virtio DMA is cache coherent; without it the HAL treats child DMA as non-coherent. |
| `_CRS` | Exactly: one `QWordMemory` for the CAM window, then one `Interrupt()` per distinct SPI. |
| `RTAB` | Flat `Package` of integer triples: `device, pin, index`. |

### `_CRS`

1. `QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite, 0, base, base+len-1, 0, len)`
   from the FDT `reg` (stock AVF: `0x2E000000`, `0x01000000`). It must be the first memory descriptor.
   The driver maps at most 16 MiB (256 buses) and needs at least 64 KiB.
2. `Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { GSIV }` for each distinct SPI in the FDT
   `interrupt-map`, GSIV = SPI + 32. Take trigger and polarity from the map's GIC flags.
   **`Shared` is required.** Each child asks for the same vector as a shared resource; an exclusive
   parent claim would block every child with Code 12.

**Do not put the PCI MMIO window (`0x2C000000`, 32 MiB) in `_CRS`.** Child BARs live inside it; a
parent claim on it conflicts with every child. The driver does not need the window: crosvm and
firmware already program the BARs, and BARs left at 0 are not reported.

### `RTAB`

`Package () { dev, pin, idx, dev, pin, idx, ... }`, all integers.

- `dev`: device number on bus 0, from `phys.hi` bits 15:11 of the `interrupt-map` child address.
- `pin`: 1 = INTA ... 4 = INTD, the `interrupt-map` child interrupt specifier.
- `idx`: zero-based position of the matching `Interrupt()` among the `_CRS` interrupt descriptors.

At most 128 triples, at most 32 interrupts. Functions behind a PCI bridge use the standard swizzle
to reach a bus 0 `(dev, pin)`. A function whose route is missing gets no interrupt resource. If
`RTAB` is missing, the bus still enumerates, without interrupts.

## What children report

| Query | Answer |
| --- | --- |
| Device ID | `PCI\VEN_vvvv&DEV_dddd&SUBSYS_ssssSSSS&REV_rr` (`ssss` subsystem ID, `SSSS` subsystem vendor) |
| Hardware IDs | that, `...&SUBSYS_ssssSSSS`, `PCI\VEN_v&DEV_d&CC_ccsspp`, `PCI\VEN_v&DEV_d&CC_ccss` |
| Compatible IDs | `PCI\VEN_v&DEV_d&REV_rr`, `PCI\VEN_v&DEV_d`, `PCI\VEN_v&CC_ccsspp`, `PCI\VEN_v&CC_ccss`, `PCI\VEN_v`, `PCI\CC_ccsspp`, `PCI\CC_ccss` |
| Instance ID | `3&0&0&BBDF`: `BB` bus, `DF` = `device << 3 \| function`, both two upper-case hex digits |
| Capabilities | `UniqueID = TRUE`, so Windows uses the instance ID verbatim. `Address = device << 16 \| function` |
| Bus info | `GUID_BUS_TYPE_PCI`, `PCIBus`, bus number |

Instance examples on bus 0: slot 1 fn 0 is `3&0&0&0008`, slot 2 is `3&0&0&0010`, slot 3 is
`3&0&0&0018`. Full Enum path: `PCI\VEN_1AF4&DEV_1042&SUBSYS_11001AF4&REV_01\3&0&0&0008`.

Bridges are not exposed. Their secondary buses are scanned and the functions behind them appear
as direct children with their own bus number.

## Interrupts

Only INTx. Children never get message-signaled resources, whatever `MSISupported` says.
The child's raw interrupt is a copy of the parent's raw `_CRS` descriptor, marked shared. The PnP
manager translates it through the same root translator that produced the parent's translated
descriptor, so the child's translated value matches the parent's.
