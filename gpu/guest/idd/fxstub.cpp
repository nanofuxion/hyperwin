// The start of a UMDF 2 driver, written here instead of linked from the WDK (wddm/README.md, "Headers and
// the WDK": no WDK static library goes into a shipped driver). WUDFHost.exe calls FxDriverEntryUm with its
// loader interface; that binds the driver to the framework (WDF's function table) and to IddCx, the
// indirect display class extension, then calls DriverEntry.
//
// The layouts below are those of the public WDF bind structures (WDF_BIND_INFO2 in Microsoft's MIT-licensed
// Windows-Driver-Frameworks, fxldr.h) and of the class bind record the framework expects for an extension,
// as UMDF 2.33 and IddCx 1.9 use them. Nothing here is copied from WDK text.
#include "hwidd.h"

extern "C" {

// ---- what wdf.h and IddCx.h expect the driver to define ----
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
const WDFFUNC *WdfFunctions_02033;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfFunctionCount = WdfFunctionTableNumEntries;
ULONG WdfStructureCount = WDF_STRUCTURE_TABLE_NUM_ENTRIES;
WDF_STRUCT_INFO WdfStructures;

PIDD_DRIVER_GLOBALS IddDriverGlobals;
PFN_IDD_CX IddFunctions[IddFunctionTableNumEntries];
BOOLEAN IddClientVersionHigherThanFramework;
ULONG IddFunctionCount = IddFunctionTableNumEntries;
ULONG IddStructureCount = IDD_STRUCTURE_TABLE_NUM_ENTRIES;
WDF_STRUCT_INFO IddStructures;
PCWSTR IddFrameworkExtensionName = L"IddCx";

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
}

// WDF_BIND_INFO2: the framework fills WdfFunctions_02033 and the counts through these pointers.
struct fx_version { ULONG major, minor, build; };
struct fx_bind_info {
    ULONG size;
    const WCHAR *component;
    fx_version version;
    ULONG func_count;
    const WDFFUNC **func_table;
    void *module;
    ULONG *min_version;
    BOOLEAN *client_higher;
    ULONG *func_count_ptr;
    ULONG *struct_count_ptr;
    WDF_STRUCT_INFO *struct_table;
};
static_assert(sizeof(fx_bind_info) == 0x58, "fx_bind_info");

// The class extension's record (version 2, 0x78 bytes): the extension fills IddFunctions and IddDriverGlobals.
struct fx_class_bind_info {
    ULONG size;
    const WCHAR *class_name;
    fx_version version;
    PFN_IDD_CX *func_table;
    ULONG func_count;
    PIDD_DRIVER_GLOBALS *globals;
    void *client_bind;
    void *client_unbind;
    void *class_module;
    ULONG *min_version;
    BOOLEAN *client_higher;
    ULONG *func_count_ptr;
    ULONG *struct_count_ptr;
    WDF_STRUCT_INFO *struct_table;
};
static_assert(sizeof(fx_class_bind_info) == 0x78, "fx_class_bind_info");
static_assert(offsetof(fx_class_bind_info, func_table) == 0x20, "func_table");
static_assert(offsetof(fx_class_bind_info, globals) == 0x30, "globals");

// WUDFHost's side: version bind for the framework, and for each class extension.
typedef NTSTATUS (*fx_version_bind)(void *context, fx_bind_info *info, PWDF_DRIVER_GLOBALS *globals);
typedef NTSTATUS (*fx_class_bind)(void *context, fx_bind_info *info, PWDF_DRIVER_GLOBALS globals, fx_class_bind_info *cls);
struct fx_loader_interface {
    ULONG size;
    fx_version_bind version_bind;      // 0x08
    void *version_unbind;              // 0x10
    fx_class_bind version_bind_class;  // 0x18
    void *version_unbind_class;        // 0x20
    void *register_class_library;      // 0x28
    ULONG flags;                       // 0x30
};
static_assert(offsetof(fx_loader_interface, flags) == 0x30, "flags");



static fx_bind_info g_bind = {
    sizeof(fx_bind_info), L"WUDFx02000", { 2, 33, 0 }, WdfFunctionTableNumEntries, &WdfFunctions_02033, nullptr,
    &WdfMinimumVersionRequired, &WdfClientVersionHigherThanFramework, &WdfFunctionCount, &WdfStructureCount, &WdfStructures,
};

static fx_class_bind_info g_idd = {
    sizeof(fx_class_bind_info), L"IddCx", { 1, IDDCX_VERSION_MINOR, 0 }, IddFunctions, IddFunctionTableNumEntries, &IddDriverGlobals,
    nullptr, nullptr, nullptr,
    &IddMinimumVersionRequired, &IddClientVersionHigherThanFramework, &IddFunctionCount, &IddStructureCount, &IddStructures,
};

extern "C" __declspec(dllexport) NTSTATUS FxDriverEntryUm(void *loader, void *context, void *driver_object, PUNICODE_STRING registry)
{
    fx_loader_interface *li = (fx_loader_interface *)loader;
    if (!li || li->size < sizeof(fx_loader_interface) - 4) return STATUS_INVALID_PARAMETER;
    NTSTATUS ns = li->version_bind(context, &g_bind, &WdfDriverGlobals);
    hwidd_log("bind wdf %08lx", (unsigned long)ns);
    if (!NT_SUCCESS(ns)) return ns;
    ns = li->version_bind_class(context, &g_bind, WdfDriverGlobals, &g_idd);
    hwidd_log("bind iddcx %08lx", (unsigned long)ns);
    if (!NT_SUCCESS(ns)) return ns;
    return DriverEntry((PDRIVER_OBJECT)driver_object, registry);
}
