# hasmanifest.ps1 <exe>: exit 1 when the program has its own manifest, 0 when it has none, 2 when unreadable.
# An external <exe>.manifest only counts for a program without one.
param([string]$exe)
$s = '[DllImport("kernel32", SetLastError=true)] public static extern IntPtr LoadLibraryEx(string f, IntPtr h, uint fl);' +
     '[DllImport("kernel32", SetLastError=true)] public static extern IntPtr FindResource(IntPtr m, IntPtr n, IntPtr t);'
$t = Add-Type -MemberDefinition $s -Name W -Namespace HW -PassThru
$m = $t::LoadLibraryEx($exe, [IntPtr]::Zero, 2)   # LOAD_LIBRARY_AS_DATAFILE
if ($m -eq [IntPtr]::Zero) { exit 2 }
if ($t::FindResource($m, [IntPtr]1, [IntPtr]24) -eq [IntPtr]::Zero) { exit 0 } else { exit 1 }
