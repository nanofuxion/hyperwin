DefinitionBlock ("", "SSDT", 2, "HYPERW", "AVFPCI", 1)
{
    Scope (\_SB)
    {
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_SEG, Zero)
            Name (_BBN, Zero)
            Name (_UID, Zero)
            Name (_CCA, One)
            /* DSM 5 = 0: keep the BAR assignments firmware made at boot. */
            Method (_DSM, 4, Serialized)
            {
                If (Arg0 == ToUUID ("E5C937D0-3553-4D7A-9117-EA4D19C3434D"))
                {
                    If (Arg2 == Zero) { Return (Buffer (One) { 0x21 }) }
                    If (Arg2 == 5) { Return (Zero) }
                }
                Return (Buffer (One) { 0x00 })
            }
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0001, , , )
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x00000000, 0x2C000000, 0x2DFFFFFF, 0x00000000,
                    0x02000000, , , )
                QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    Prefetchable, ReadWrite,
                    0x0000000000000000, 0x0000000188000000, 0x000000FFFFFFFFFF,
                    0x0000000000000000, 0x000000FE78000000, , , )
            })
        }
    }
}
