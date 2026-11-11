# What a page sees

The host exists so that a page cannot tell it is driven. A client keeps that property by installing no
page-world script; everything below holds without one.

- The view is visible, focused and active, and its input is trusted. `navigator.webdriver` is false.
- Nothing is composited or painted to a buffer except during `capture` and while the block is presented, while
  animation frames keep their display cadence ([presentation.md](presentation.md), "Pacing").
- `webkit.messageHandlers.stencil` and `stencilFrame` exist in the isolated world always, and in the page's
  world only while the `scripts` list holds a page-world script.
- The screen and the window are never the viewport. The window frame is `x: 80, y: 60`, as wide as the
  viewport and 87 px taller; the screen is the first of 1920×1080, 2560×1440, 3440×1440 and 3840×2160 that
  holds it, less a 25 px menu bar at the top on macOS and a 40 px bar at the bottom elsewhere. Both follow the
  viewport.
- The machine is a desktop: there is a mouse and a keyboard and no touchscreen, so `(hover: hover)` and
  `(pointer: fine)` match and there are no touch events or `ontouch*` attributes.
- On macOS the user agent is Safari's and `window.safari` exists. The generic font families are the ones Safari
  uses (Times, Helvetica, Courier, Apple Chancery, Papyrus; Times for a family that is not installed), and
  `system-ui` and `-apple-system` are the system font. Form controls use the system font at 11 px.
  Scrollbars overlay the page and take no room in it, as on a Mac with only a trackpad.
- On macOS and Linux the TLS handshake, the HTTP/2 settings and the request headers, their order included, are
  the ones Safari sends (on Linux under the engine's own user agent). Windows uses MSYS2's stock TLS.
- Video and audio play and Web Audio runs, but no sound reaches an output device. There is no camera or
  microphone: `enumerateDevices()` lists one unlabelled device of each kind, as Safari does before permission,
  and `getUserMedia()` and `getDisplayMedia()` are denied. WebAuthn requests and `navigator.share()` end as a
  sheet the user dismissed, and no gamepad is attached.
- `file:` documents cannot read other files until the client sends `fileAccess` ([protocol.md](protocol.md)).
- `identity` changes this: `seed` (a string; `null` for none) seeds the noise the engine adds to canvas
  read-backs, so one seed is one stable device; `screen` (`{"width","height","availLeft","availTop","availWidth",
  "availHeight"}`) and `window` (`{"x","y","width","height"}`) pin those values, and `null` returns either to
  the rule above. A member that is left out is unchanged.

Which engine features each platform's build has: [building.md](building.md).
