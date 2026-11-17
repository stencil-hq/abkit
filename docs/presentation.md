# Presentation

`{"op":"present","block":N,"to":TARGET}` shows a block's frames in another process without copying them:
while presented, the block renders like a page on screen and every frame it renders goes to the target (on
macOS as the buffer the web process drew it into; on Windows into the target's window). Its `cursor` events are
reported only while it is presented. `{"op":"present","block":N,"to":null}` stops. Stopping, `close`, the
host's exit and (on macOS) the target's death end a presentation; then the buffers go back to the view and
nothing is rendered again until the next `capture` or `present`. `capture` works while presenting. Presenting a
block to the target it is presented to already does nothing; to another one, ends the first presentation.

`TARGET`:

- macOS: `{"mach":NAME}`, a Mach bootstrap service the client checked in (`bootstrap_check_in()`), which must
  outlive the presentations to it. One service serves every block, and every host, of a client. The answer
  `{}` comes once the service was reached and sent `hello`; `failed` when it cannot be reached.
- Windows: `{"hwnd":N}`, a window of the client's ([below](#windows)). The answer `{}` comes once the block's
  view was given the window; `invalid` when `N` is not a number or no window.
- `{"socket":PATH}` (Linux) is reserved: answered `unsupported`. Each system answers the other's targets
  `unsupported`.

A client that wants windows without speaking the Mach channel itself starts the engine's viewer
([viewer.md](viewer.md)) and presents blocks to it.

## Pacing

The host's pages run their animation frames at the refreshes of one screen. While a client sent a `tick` (its
display refreshed) within the last second, a refresh is a `tick`; otherwise a 60 Hz timer is. A client sends
`tick`s from its display link while it shows a presented block, and stops when it shows none. A presented frame
counts as shown at the refresh after it was sent (its page's next frame waits for that), so a presented page
renders at most one frame per refresh: with 120 ticks a second, 120 frames and animation frames a second.

## Windows

`N` is an `HWND` of the client's, as a JSON number: the window that shows the page, of the page's size in device
pixels (the block's CSS size times its `scale`). The block's web process presents its frames straight into it,
with a swap chain of its own, in a child window the host makes inside it (sized to the page, at its top left;
disabled and transparent to hits, so all input goes to window `N`). Nothing is sent to the client: no channel,
no buffers, no `tick`s; the swap chain paces the page (one frame per refresh of the display). The client keeps
the page's size and window `N`'s in step (`viewport`); a client giving `N` the `WS_CLIPCHILDREN` style keeps
its own painting off the page's frames.

When the web process cannot present into a window (no hardware acceleration, no desktop composition, or the
swap chain could not be made), its frames come to the host as buffers, and the host shows them in window `N`
itself: in a child window of its own there, of the frame's size at the top left, below the web process's (whose
frames cover it), disabled and transparent to hits as well. Either way the client sees the page's frames in `N`.

A web process that crashes in the graphics stack (a display driver, Direct3D or ANGLE, as the host's web process
extension finds) costs the host the GPU: from then on its pages draw in software, their frames coming as buffers
as above. Each page that drew with the GPU and crashed loads again, reporting no `failed`; pages opened later
draw in software from the start, and pages still running keep the GPU until they crash too. That lasts until
the host exits.

While a `capture` is under way the web process presents nothing into the window, which keeps the frame it last
showed. `"to":null`, `close`, or presenting into another window ends the presentation: window `N` keeps the
last frame the web process presented into it (the host's child window for it stays until the block closes), and
the host's own window for frames as buffers goes. The host takes its windows' messages on its main loop from the
first presentation into a window on (a client's thread sends its child windows messages, and waits for them).

The client may destroy window `N` (with it the host's child windows go) before its `close` or `"to":null` comes:
the host and the web process carry on, nothing is shown, and the presentation ends when the client says so.

## The Mach channel (macOS)

Plain `mach_msg()` messages between the host and the client's service, in native byte order. `msgh_id` is the
kind. A simple message is the 24-byte `mach_msg_header_t` followed by its payload of 32-bit unsigned words; a
complex one (`MACH_MSGH_BITS_COMPLEX`) is the header, a `mach_msg_body_t` (4 bytes, `msgh_descriptor_count`
1), one `mach_msg_port_descriptor_t` (12 bytes) and then its payload words. `msgh_size` is exactly that size:

| `msgh_id` | Name | Direction | Port descriptor | Payload (32-bit words) | `msgh_size` |
|---|---|---|---|---|---|
| 1 | `hello` | host → client | a send right (`MACH_MSG_TYPE_MAKE_SEND`) to the host's port for this client | `version` (1), `pid` of the host | 48 |
| 2 | `buffer` | host → client | the buffer's IOSurface (`IOSurfaceCreateMachPort()`, moved) | `block`, `buffer`, `width`, `height` (device px) | 56 |
| 3 | `frame` | host → client | | `block`, `buffer` | 32 |
| 4 | `gone` | host → client | | `block`, `buffer` | 32 |
| 5 | `bye` | host → client | | `block` | 28 |
| 16 | `released` | client → host | | `block`, `buffer` | 32 |
| 17 | `tick` | client → host | | none | 24 |

Byte offsets: in a simple message the words start at 24 (`block` at 24, `buffer` at 28); in a complex one the
port name is at 28 (the descriptor's `name`) and the words start at 40 (`hello`: `version` at 40, `pid` at 44;
`buffer`: `block` at 40, `buffer` at 44, `width` at 48, `height` at 52). The client's messages go to the port
`hello` gave, with `MACH_MSG_TYPE_COPY_SEND` and no reply port.

- `hello` is the first message a host sends to a service, once per host process. Its port takes the client's
  `released` and `tick`s for every block of that host.
- `buffer` comes before the first `frame` in that buffer. The client looks the surface up
  (`IOSurfaceLookupFromMachPort()`) and deallocates the right. Buffer ids are per block and never reused; a
  block makes new buffers when its size or scale changes, or when its web process is replaced.
- A surface is 32-bit BGRA, premultiplied alpha, sRGB, top row first, `width` × `height` device pixels (the
  block's CSS size times its `scale`). After `frame` the client shows it (e.g. as a `CALayer`'s contents), and
  sends `released` for it once it no longer does (typically when the next `frame` replaces it): the host draws
  into a buffer again only after that. A client reading pixels does so under `IOSurfaceLock()`, which waits for
  the GPU to finish drawing.
- `gone`: the host destroyed that buffer; the client lets go of its surface (not sending `released` for it).
- `bye`: the presentation of that block ended; all its buffers are gone, released or not.
- The host sends with a one-second timeout: a client keeps its service's queue large enough
  (`MACH_PORT_QLIMIT_LARGE`) and drains it. A client that dies, or whose queue stays full, is gone: its
  presentations end as with `"to":null`, without `bye`.
