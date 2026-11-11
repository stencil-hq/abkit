# The viewer (macOS)

The host has no window. `ABView`, beside the host in the engine's directory, is a presentation client
([presentation.md](presentation.md)) that gives presented blocks one each: a program a client starts, takes its
service name from, and presents blocks to. It links nothing of the engine. It speaks one JSON object a line on its
standard streams and exits when its input closes.

- It writes `{"ready":{"mach":NAME}}` once its service (`so.stencil.ABView.<pid>`) is checked in: the `present`
  target is `{"mach":NAME}`. One viewer serves the blocks of one host.
- `{"show":{"block":N,"title":"…","scale":S}}` names a block's window before the block is presented: its title
  and the block's `scale`, from which the window takes its size (one point per CSS pixel, smaller where the
  screen is). The window opens with the block's first frame, in front but without taking the keyboard from the
  app the user is in, and closes when the presentation ends. `show` for a shown block renames its window.
- `{"hide":{"block":N}}` closes a block's window; the client also stops presenting the block.
- `{"input":{"block":N,"events":[…]}}` is what the user did in a window, as steps of the `input` op in the
  block's CSS pixels (`mouse`, `wheel`, `key`, and `text` for what a key press cannot say); the client sends
  them on. The viewer never sets `focus`: a shown page stays focused like a hidden one.
- `{"closed":{"block":N}}`: the user closed the window; the client stops presenting the block.
  `{"ended":{"block":N}}`: the block's presentation ended (`bye`) and its window is gone.

A typical client (omp's `webkit/host.ts`): start `ABView` with the first window, wait for `ready`, send `show`,
then `{"op":"present","block":N,"to":{"mach":NAME}}` to the host; forward `input` lines to the host's `input`
op; on `closed` send `present` with `"to":null`; close the viewer's input when the last window is gone.
