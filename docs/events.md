# Events

What the `events` op ([protocol.md](protocol.md)) returns for a block. Every event has `type` and `seq`.

## Page and navigation

`url`, `title`, `progress` (`value`), `history` (`back`, `forward`), `committed` (`url`), `loaded` (`url`),
`failed` (`message`), `newWindow` (`url`; nothing opens), `blocked` (`url`, a navigation `allow` refused),
`dialog` (`kind`, `message`, `default`, `url`, `handled`: `accepted`, `dismissed` or `null` while held),
`chooser` (`multiple`, `directories`, `handled`), `download` (`id`, `url`, `path`, `state`: `started`,
`finished`, `failed`, `error`), `message` (`body`, `world`, `main`, `url`; a script posted to
`webkit.messageHandlers.stencil`), `frame` (`name`, `main`, `url`; posted to `stencilFrame`) and `cursor`
(`name`, a CSS cursor keyword such as `default`, `pointer`, `text`: the page's cursor changed, reported only while
the block is presented, and once when a presentation starts).

## Observed

Reported without any script in the page, each with `ts` (epoch milliseconds). The welcome's `console` and
`network` say these are reported.

- `console`: `level` (`log`, `info`, `warning`, `error`, `debug`), `text`, `source` (`javascript` for an
  uncaught exception, `console-api`, `network`, `security`, `other`), `url`, `line`.
- `request`: `id` (unique in the block), `url`, `method`, `headers`, `main`. A redirect repeats it with the
  same `id`.
- `response`: `id`, `url`, `status`, `mime`, `headers`, `main`.
- `requestFinished`: `id`, `length` (the declared `Content-Length`, 0 when there is none).
- `requestFailed`: `id`, `url`, `error`.
