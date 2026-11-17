/* Example HWPC0001 node. The loader generates the real one from the live FDT;
 * every number marked "from FDT" below is a placeholder. CONTRACT.md has the rules. */
DefinitionBlock ("hwpci.aml", "SSDT", 2, "HYPWIN", "HWPCI", 1)
{
    Scope (\_SB)
    {
        Device (PCI0)
        {
            Name (_HID, "HWPC0001")
            Name (_UID, 0)
            Name (_CCA, One)
            Name (_CRS, ResourceTemplate ()
            {
                /* CAM window, from FDT pci-host-cam-generic "reg". */
                QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                    0, 0x2E000000, 0x2EFFFFFF, 0, 0x01000000)
                /* One per distinct SPI in the FDT interrupt-map, GSIV = SPI + 32. Order defines RTAB index. */
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 36 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 37 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 38 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 39 }
            })
            /* Flat triples: PCI device number on bus 0, INTx pin (1 = INTA .. 4 = INTD),
             * zero-based index of the Interrupt() descriptor above. From FDT interrupt-map. */
            Name (RTAB, Package ()
            {
                1, 1, 0,
                2, 1, 1,
                3, 1, 2,
                4, 1, 3,
            })
        }
    }
}
