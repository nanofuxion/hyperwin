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

## Touch screen and touchpad (added 2026-11-18)

Asked for in the same phase: true multi-touch, and a touchpad with proper
gestures. Both come from a new driver instead of the app imitating them.

- **`hwhid.sys`**, a HID minidriver (`guest/drivers/hwhid/hwhid.c`) on a root
  device `Root\HwHid`. Windows sees two inbox devices on it: a 10-finger
  "HID-compliant touch screen" and a Windows Precision Touchpad (5 fingers,
  click-pad), plus the configuration and mouse collections a touchpad must
  have. The certification blob is Microsoft's sample, which Windows accepts
  from a device without a signed one.
- **Its own vsock port**, `HW_VSOCK_PORT_HID` (5652). `HidInput.kt` sends every
  finger with its id and whether it still touches.
- **Touch mode** sends the fingers to the touch screen. Windows then does press
  and hold, edge swipes and pinch itself, and touch works on the sign-in and
  UAC screens too. The agent's injected touch stays as the fallback until the
  driver answers.
- **Trackpad mode** makes the phone's screen the touchpad. Windows does tap to
  click, tap and drag, two-finger scroll, pinch and the three and four finger
  swipes, with its own touchpad settings. The app's hand-made trackpad stays
  as the fallback.
- **Install** goes the same way as sound: `install-driver.ps1 hwhid
  Root\HwHid` from `gpu-install.cmd`, which now serves both drivers. The driver
  keeps itself loaded, so an update to it takes effect at the next restart.

## As built (2026-11-18)

- Message 4 and the longer message 2 are as planned. Wheels are 1/120 steps
  from `AXIS_VSCROLL` and `AXIS_HSCROLL`, history included, remainders carried.
- Agent version 5: the touch helper checks the cursor every 100 ms and reports
  `HW_AGENT_CURSOR` through a pipe back to the service. "Not showing" counts
  both hidden and suppressed: after any touch input Windows reports the cursor
  as suppressed, not hidden. A mouse that moves shows it again, and then the
  app lets go.
- The toolbar has a "Game mouse" button. Settings has Automatic, Always, Never
  and the speed slider. Ctrl + Alt lets go, until Windows shows the cursor again.
- Debug builds take `am broadcast -a dev.hyperwin.app.MOUSE --ei dx --ei dy
  --ei buttons --ei wheel --ei hwheel --ei n`, and `guest/tools/inputtest.exe
  [seconds] [look]` shows what Windows receives.

Measured on the Pixel: 96 relative packets of (-5, 3) arrived as exactly
-480, 288 in raw input. Wheel 4 x 60 gave 240, hwheel -120, back button seen.
A touch swipe gave 41 touch updates. A touchpad drag moved the cursor, and a
90 ms tap clicked. Auto capture took the mouse 1 s after a hidden, clipped,
full-screen window opened, and let go when it closed.

Not checked yet, needing a physical mouse or two hands: captured movement from
a real mouse, a shooter turning 360°, two-finger gestures, capture in split
screen, and how fast a high-rate mouse goes.

## Goal now: no separate "game mouse" (2026-11-23)

The aim is a mouse that needs no mode. Every pointer on the phone works in games and on the
desktop without a button to press. What changed in the app, not yet committed:

- **The trackpad is a relative mouse.** `VsockInput.trackpad` sends movement as mouse counts
  (message 4), with the fraction carried to the next event. Taps, tap and drag, and two finger
  scroll go the same way. Phone pixels are scaled to Windows pixels, so one drag across the view
  crosses Windows' screen. Games that hide the cursor still turn with it. The Precision Touchpad
  path (`HidInput.pad`) is no longer used for this mode.
- **Automatic capture is gone.** It held the mouse whenever a game hid the cursor, and kept
  holding it after the game closed. The agent's cursor report is no longer needed for this.

Still to do before the toolbar's "Game mouse" button and the Automatic/Always/Never setting can
go:

1. A real mouse on the phone: does `SOURCE_MOUSE` without capture reach games as relative
   counts? If not, capture only the real mouse, not touch, and release it when the app loses focus.
2. Remove the button, the setting, the `GAME_MOUSE` keys and the Ctrl + Alt chord, or keep the
   chord as a plain way to free a stuck mouse.
3. Test a shooter turning 360° with the trackpad, and the desktop with a touchpad and a mouse.
4. Decide what happens to Windows' own touchpad gestures. The trackpad no longer gives three and
   four finger swipes. Either send them as keys, or keep an option for the old Precision Touchpad.

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
