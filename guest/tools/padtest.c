// Shows what XInput and the HID game controller (through WinMM's joystick calls, as joy.cpl and
// DirectInput-era games see it) get of HyperWin's controllers (gamepad/README.md).
//   padtest.exe [seconds] [player left right]   default 3 s; with a player (1 to 4) also rumbles it
// Writes padtest.txt beside the program, one line a second: each XInput user index, connected or
// not, its packet number, buttons and left stick X.
#include <windows.h>
#include <xinput.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>

typedef DWORD(WINAPI *GetState)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *SetState)(DWORD, XINPUT_VIBRATION *);

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int seconds = argc > 1 ? atoi(argv[1]) : 3;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "padtest.txt");
    FILE *out = fopen(path, "w");
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    GetState get = (GetState)GetProcAddress(x, "XInputGetState");
    SetState set = (SetState)GetProcAddress(x, "XInputSetState");
    if (!get || !set) { fprintf(out, "FAIL no xinput1_4\n"); return 1; }
    if (argc > 4) {
        XINPUT_VIBRATION v = {(WORD)(atoi(argv[3]) * 257), (WORD)(atoi(argv[4]) * 257)};
        DWORD r = set((DWORD)atoi(argv[2]) - 1, &v);
        fprintf(out, "rumble player %s: %s %s -> %lu\n", argv[2], argv[3], argv[4], r);
    }
    for (int s = 0; s < seconds; s++) {
        fprintf(out, "%2d s:", s + 1);
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE st = {0};
            DWORD r = get(i, &st);
            if (r == ERROR_SUCCESS)
                fprintf(out, "  [%lu] packet %lu buttons %04x lx %d", i, st.dwPacketNumber, st.Gamepad.wButtons, st.Gamepad.sThumbLX);
            else
                fprintf(out, "  [%lu] none", i);
        }
        for (UINT j = 0; j < 4; j++) {
            JOYINFOEX ji = {sizeof ji, JOY_RETURNALL};
            if (joyGetPosEx(j, &ji) == JOYERR_NOERROR)
                fprintf(out, "  joy%u x %lu z %lu buttons %lx pov %lu", j, ji.dwXpos, ji.dwZpos, ji.dwButtons, ji.dwPOV);
        }
        fprintf(out, "\n");
        fflush(out);
        Sleep(1000);
    }
    if (argc > 4) {
        XINPUT_VIBRATION v = {0, 0};
        set((DWORD)atoi(argv[2]) - 1, &v);
    }
    return 0;
}
