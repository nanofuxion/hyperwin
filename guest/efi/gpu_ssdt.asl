/* The HyperWin GPU as a platform device, so its WDDM driver (hwgpuw.sys) gets an interrupt line.
 * Nothing raises it yet: the driver polls. dxgkrnl only runs a driver's routines "at interrupt level"
 * (DxgkCbSynchronizeExecution, and the flips of a display) when the adapter has an interrupt, so a
 * root device without one could not show the desktop. GSIV 63 (SPI 31) is unused on AVF. */
DefinitionBlock ("", "SSDT", 2, "HYPRWN", "HWGPU", 1)
{
    Scope (\_SB)
    {
        Device (HGPU)
        {
            Name (_HID, "HWGP0001")
            Name (_UID, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 63 }
            })
        }
    }
}
