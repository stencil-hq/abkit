# Presentation

`{"op":"present","block":N,"to":TARGET}` shows a block's frames in another process without copying them:
while presented, the block renders like a page on screen and every frame it renders goes to the target as the
buffer the web process drew it into. `{"op":"present","block":N,"to":null}` stops. Stopping, `close`, the
host's exit and the target's death end a presentation; then the buffers go back to the view and nothing is
rendered again until the next `capture` or `present`. `capture` works while presenting. Presenting a block to
the target it is presented to already does nothing; to another one, ends the first presentation.

`TARGET`:

- macOS: `{"mach":NAME}`, a Mach bootstrap service the client checked in (`bootstrap_check_in()`), which must
  outlive the presentations to it. One service serves every block, and every host, of a client. The answer
  `{}` comes once the service was reached and sent `hello`; `failed` when it cannot be reached.
- `{"hwnd":N}` (Windows) and `{"socket":PATH}` (Linux) are reserved: answered `unsupported`.

A client that wants windows without speaking the Mach channel itself starts the engine's viewer
([viewer.md](viewer.md)) and presents blocks to it.

## Pacing

The host's pages run their animation frames at the refreshes of one screen. While a client sent a `tick` (its
display refreshed) within the last second, a refresh is a `tick`; otherwise a 60 Hz timer is. A client sends
`tick`s from its display link while it shows a presented block, and stops when it shows none. A presented frame
counts as shown at the refresh after it was sent (its page's next frame waits for that), so a presented page
renders at most one frame per refresh: with 120 ticks a second, 120 frames and animation frames a second.

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
