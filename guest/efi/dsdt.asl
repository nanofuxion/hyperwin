DefinitionBlock ("", "DSDT", 2, "HYPERW", "HYPERWIN", 1)
{
    Scope (\_SB)
    {
        /* Windows enumerates processors from the namespace; _UID matches the MADT GICC UID. */
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, Zero)
        }
    }
}
