/* Players as USB Xbox 360 controllers on the pad device (usbpad.c). */
#pragma once
#define USBPAD_SLOTS 4

void usbpad_init(PDRIVER_OBJECT driver, void (*rumble)(ULONG slot, UCHAR big, UCHAR small));
void usbpad_set_bus(PDEVICE_OBJECT pdo);
BOOLEAN usbpad_is_child(PDEVICE_OBJECT device);
NTSTATUS usbpad_dispatch(PDEVICE_OBJECT device, PIRP irp);
PDEVICE_RELATIONS usbpad_relations(void);
void usbpad_update(ULONG slot, BOOLEAN ds4, BOOLEAN connected, USHORT buttons, UCHAR lt, UCHAR rt, SHORT lx, SHORT ly, SHORT rx, SHORT ry);
