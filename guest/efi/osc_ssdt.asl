/* Platform-wide _OSC: Windows only uses a processor's _CPC (its performance description) after the
 * platform grants CPPC here. Everything else Windows offers is declined. */
DefinitionBlock ("", "SSDT", 2, "HYPRWN", "HWOSC", 1)
{
    Scope (\_SB)
    {
        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            If (Arg0 == ToUUID ("0811b06e-4a27-44f9-8d60-3cbbc22e7b48"))
            {
                CreateDWordField (Arg3, 4, CDW2)
                CDW2 &= 0x60            /* bit 5: CPPC, bit 6: CPPC revision 2 */
            }
            Else
            {
                CDW1 |= 4               /* unrecognized UUID */
            }
            Return (Arg3)
        }
    }
}
