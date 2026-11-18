# Relative mouse for games

Shooters and other mouse-look games get raw mouse movement, the way they do on
a PC. The desktop keeps working as it does now.

## Why mouse-look breaks today

`VsockInput.mouse` turns each Android mouse event into a position on the guest
screen. `hwvsinput.sys` delivers it as `MOUSE_MOVE_ABSOLUTE`
(`hwvsinput.c:112`). A mouse-look game hides the cursor and reads movement. It
often also snaps the cursor back to the middle with `SetCursorPos` every frame.
With absolute positions:

- the mouse stops at the edge of the phone's screen, so you can't keep turning;
- raw input (`WM_INPUT`) reports absolute packets, and many games ignore those;
- the game's recentring fights the phone's positions, so the view jumps or spins.

Smaller gaps in the same path:

- `AXIS_VSCROLL` is cut to a whole number (`VsockInput.kt:247`), so the small
  steps from a smooth-scrolling mouse or touchpad come out as 0.
- No horizontal wheel, and no back and forward buttons (X1 and X2). Only left,
  right and middle are mapped.

## What to build

### Android: pointer capture

- `GuestView.requestPointerCapture()` while "game mouse" is on.
  `setOnCapturedPointerListener` then gets every mouse event as
  `SOURCE_MOUSE_RELATIVE`, where `AXIS_X` and `AXIS_Y` are the movement since
  the last event. The cursor stops being bound to the phone's screen.
- Add up all historical samples in each event (`getHistoricalAxisValue`), so
  fast flicks lose nothing. `requestUnbufferedDispatch(SOURCE_ANY)` is already
  set on `GuestView`, so events aren't held for the next frame.
- Keep the fractions. Carry what's left after rounding into the next event, so
  slow, precise moves still add up.
- Android drops capture when the app loses focus. `onPointerCaptureChange`
  tells the app, and it switches back to absolute.

### When to capture

1. **Automatically, from Windows.** The agent's helper already runs in the
   user's session (`hwagent.exe touch`). Every 100 ms it checks
   `GetCursorInfo` (is the cursor hidden?) and `GetClipCursor` (is it held
   inside a window?) for the foreground window. "Hidden, and clipped or
   fullscreen" means a game wants mouse-look. It sends `HW_AGENT_CURSOR {hidden,
   clipped}` to the app. The app captures on that, and releases when the cursor
   comes back.
2. **By hand.** A "Game mouse" button in the toolbar, and a key chord to get
   out, Ctrl + Alt like most VM apps. The app always takes that chord, so a
   game that never shows its cursor again can't trap the mouse.
3. A setting: Automatic (the default), Always, or Never.

### The message

A new message type 4, relative pointer:

```
[len] [4] [dx s16] [dy s16] [buttons] [wheel s16] [hwheel s16]
```

- Movement is in mouse counts, the same as a PC mouse reports.
- Buttons: bit 0 left, 1 right, 2 middle, 3 back, 4 forward.
- The wheel is in 1/120 steps (`WHEEL_DELTA`), so smooth scrolling keeps its
  fractions. Fold the same wheel and button changes into message 2 by
  extending it with an hwheel field and X buttons. Older drivers ignore the
  extra bytes.
- `dx` and `dy` that don't fit in 16 bits are split across messages.

### Windows: `hwvsinput.sys`

- Message 4 becomes `MOUSE_INPUT_DATA` with `MOUSE_MOVE_RELATIVE`, `LastX` and
  `LastY` set to the movement.
- Back and forward buttons map to `MOUSE_BUTTON_4_DOWN/UP` and `_5_`.
  `MOUSE_HWHEEL` handles the horizontal wheel.
- Relative and absolute packets can both come from the one mouse device.
  Windows handles each packet by its flag.
- Raw input games then get real counts. Windows' "Enhance pointer precision"
  only changes the desktop cursor, not raw input. That matches a PC.

### Sensitivity

Android's captured movement is in the same counts the mouse sends, so leave it
alone by default. Add a "Mouse speed in games" slider (0.25× to 4×) for
high-DPI mice. Touch trackpad mode can use message 4 too, which fixes
trackpad-look in games for people without a mouse.

## Phases

1. Message 4 in the app and driver, with a manual toggle. Test: a raw-input
   test tool shows real counts, and a shooter turns smoothly through 360° and
   past.
2. Wheel steps and buttons 4 and 5. Test: smooth scrolling in Edge with a
   touchpad, and back and forward in File Explorer.
3. Automatic capture from the agent's cursor report, and the escape chord.
   Test: a game's menu (cursor shown) and gameplay (hidden) switch cleanly, and
   alt-tab gives the cursor back.
4. Sensitivity slider and trackpad-look.

## Things to check early

- Pointer capture while the app is in Android's split screen or freeform
  window mode. Capture may be refused there.
- How fast a high-rate mouse sends events through Android on the Pixel. Check
  that one vsock message per event keeps up, or batch to one message per
  millisecond.
