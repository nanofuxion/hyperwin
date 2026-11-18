/* Prints what XInput sees for controller 0 for a few seconds: whether it is there, its
 * capabilities, and each change of state. The gamepad check for hwvsinput.sys.
 *   xitest.exe [seconds]                                                                      */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    XINPUT_CAPABILITIES caps;
    DWORD r = XInputGetCapabilities(0, 0, &caps);
    printf("capabilities: %lu", r);
    if (r == ERROR_SUCCESS)
        printf(" type %u subtype %u buttons 0x%04x", caps.Type, caps.SubType, caps.Gamepad.wButtons);
    printf("\n");
    DWORD last = 0xffffffff;
    for (int i = 0; i < seconds * 100; i++) {
        XINPUT_STATE s;
        r = XInputGetState(0, &s);
        if (r != ERROR_SUCCESS) {
            if (last != 0xfffffffe) printf("state: %lu (not connected)\n", r);
            last = 0xfffffffe;
        } else if (s.dwPacketNumber != last) {
            last = s.dwPacketNumber;
            printf("packet %lu buttons 0x%04x lt %u rt %u lx %d ly %d rx %d ry %d\n", s.dwPacketNumber,
                   s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX,
                   s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
        }
        Sleep(10);
    }
    r = XInputGetCapabilities(0, 0, &caps);
    printf("capabilities again: %lu\n", r);
    return 0;
}
