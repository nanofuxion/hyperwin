/* Bootstrap XInput. Anti-cheat titles should use a virtual HID pad instead of this DLL.
 * The pad state is the shared-window mailbox the AVF app publishes, same bytes as vsock port 5601.
 */
#if defined(HYPERWIN_NO_WINDOWS)
#include "win.h"
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "map.h"

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION;

typedef struct {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;

#define XINPUT_DEVTYPE_GAMEPAD 0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD 0x01

static BOOL enabled = TRUE;

static DWORD fill(DWORD index, XINPUT_STATE *state, int keep_guide)
{
    struct hw_header *h = hw_guest_window();
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (index != 0 || !h || !__atomic_load_n(&h->pad_connected, __ATOMIC_ACQUIRE))
        return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = h->pad_packet;
    state->Gamepad.wButtons = enabled ? h->buttons : 0;
    if (!keep_guide) state->Gamepad.wButtons &= (WORD)~0x0400;
    state->Gamepad.bLeftTrigger = enabled ? h->left_trigger : 0;
    state->Gamepad.bRightTrigger = enabled ? h->right_trigger : 0;
    state->Gamepad.sThumbLX = enabled ? h->left_x : 0;
    state->Gamepad.sThumbLY = enabled ? h->left_y : 0;
    state->Gamepad.sThumbRX = enabled ? h->right_x : 0;
    state->Gamepad.sThumbRY = enabled ? h->right_y : 0;
    return ERROR_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state)
{
    return fill(index, state, 0);
}

__declspec(dllexport) DWORD WINAPI XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    return fill(index, state, 1);
}

__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    (void)vibration;
    if (index != 0 || !hw_guest_window()) return ERROR_DEVICE_NOT_CONNECTED;
    return ERROR_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    struct hw_header *h = hw_guest_window();
    (void)flags;
    if (!caps) return ERROR_BAD_ARGUMENTS;
    if (index != 0 || !h || !h->pad_connected) return ERROR_DEVICE_NOT_CONNECTED;
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Flags = 0;
    caps->Gamepad.wButtons = 0xf3ff;
    caps->Gamepad.bLeftTrigger = 0xff;
    caps->Gamepad.bRightTrigger = 0xff;
    caps->Gamepad.sThumbLX = (SHORT)0xffc0;
    caps->Gamepad.sThumbLY = (SHORT)0xffc0;
    caps->Gamepad.sThumbRX = (SHORT)0xffc0;
    caps->Gamepad.sThumbRY = (SHORT)0xffc0;
    caps->Vibration.wLeftMotorSpeed = 0;
    caps->Vibration.wRightMotorSpeed = 0;
    return ERROR_SUCCESS;
}

__declspec(dllexport) void WINAPI XInputEnable(BOOL enable)
{
    enabled = enable;
}

BOOL WINAPI DllMain(void *module, DWORD reason, void *reserved)
{
    (void)module;
    (void)reason;
    (void)reserved;
    return TRUE;
}
