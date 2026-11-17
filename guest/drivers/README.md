# Guest drivers

Built on macOS with clang (`--target=aarch64-w64-mingw32`), mingw-w64 DDK headers, and the
ARM64 shim in `shim/armddk.h`. Import libraries come from `.def` files via `llvm-dlltool -m arm64`.

`drivers.json` lists every driver the machine prep step copies into the offline Windows
directory and registers in the offline SYSTEM hive. Each entry is the contract between the
driver author and the prep code:

- `service`, `file` (built path, relative to repo root), `start` (0 boot, 3 demand),
  `type` (1 kernel), `group` (optional), `class` (setup class GUID)
- `devices`: device instances to pre-create under `Enum`, each with `enumerator\deviceid\instance`, optional per-device `class`, `className`, `description`,
  plus `hardwareIds`. Pre-created instances carry `Service` and `ConfigFlags=0`, so the kernel
  binds them at boot without an INF install.
- `values` (optional): extra registry values, `REG_DWORD` for numbers and `REG_SZ` for strings. `service` maps a subkey of
  `Services\<service>` to `{name: value}`. `device` maps a subkey of each pre-created `Enum`
  instance to `{name: value}`. These stand in for the INF `AddReg` sections we skip.
