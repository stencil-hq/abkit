#!/usr/bin/env python3
"""Smoke-tests a packaged ABKit engine directory (package-engine*.sh output).

Starts the engine's ABHost (ABHost.exe on Windows), speaks its protocol
(docs/protocol.md) over its standard streams, and checks what every client
relies on: the welcome, a page that loads, script that runs in it, its images
painted, a capture with pixels, and a clean exit once the host's input ends. Prints one line per
step; exits non-zero on the first failure.

Usage: smoke-engine.py ENGINE_DIR
"""
import base64
import json
import os
import struct
import subprocess
import sys
import threading
import time
import zlib

TIMEOUT = 60

# Four small raster images, so the GPU painter packs them into one atlas texture (it does for four or
# more of 8 to 512 px). On macOS that texture once had no storage and every such image painted black.
IMAGE_COLORS = [(0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255)]
IMAGE_SIZE = 16


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def solid_png(color):
    """An opaque IMAGE_SIZE square PNG of one colour."""
    header = struct.pack(">IIBBBBB", IMAGE_SIZE, IMAGE_SIZE, 8, 2, 0, 0, 0)
    rows = (b"\0" + bytes(color) * IMAGE_SIZE) * IMAGE_SIZE
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b"")


def png_rows(data):
    """Decodes an 8-bit, non-interlaced RGB or RGBA PNG (a capture) into rows of (r, g, b) tuples."""
    position, compressed = 8, b""
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color_type not in (2, 6) or interlace:
                raise RuntimeError(f"capture: PNG of depth {depth}, colour type {color_type}, interlace {interlace}")
            channels = 3 if color_type == 2 else 4
        elif kind == b"IDAT":
            compressed += body
        position += 12 + length
    raw = zlib.decompress(compressed)
    stride = width * channels
    rows, previous = [], bytes(stride)
    for y in range(height):
        start = y * (stride + 1)
        method, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if method == 1:
                line[i] = (line[i] + left) & 255
            elif method == 2:
                line[i] = (line[i] + up) & 255
            elif method == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif method == 4:
                guess = left + up - corner
                distances = (abs(guess - left), abs(guess - up), abs(guess - corner))
                predictor = left if distances[0] <= min(distances[1:]) else up if distances[1] <= distances[2] else corner
                line[i] = (line[i] + predictor) & 255
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        previous = line
    return rows


PAGE = (
    "data:text/html,<title>smoke</title><body style='background:%23f00;margin:0'><div style='display:flex'>"
    + "".join(f"<img src='data:image/png;base64,{base64.b64encode(solid_png(color)).decode()}'>" for color in IMAGE_COLORS)
    + "</div><p id=x>hello</p>"
)

# Keep an older context's commands unsubmitted while another context reads back its
# pixels. A display-wide queue must not wait for the unrelated older command buffer.
READBACK = """
() => {
    const canvases = [document.createElement('canvas'), document.createElement('canvas')];
    for (const canvas of canvases) { canvas.width = 4; canvas.height = 4; }
    document.body.replaceChildren(...canvases);
    const contexts = canvases.map(canvas => canvas.getContext('webgl', { preserveDrawingBuffer: true }));
    if (contexts.some(gl => !gl)) throw new Error('WebGL unavailable');
    const pixel = new Uint8Array(4);
    for (let iteration = 0; iteration < 64; ++iteration) {
        for (let index = 0; index < contexts.length; ++index) {
            const gl = contexts[index];
            gl.clearColor(index, iteration % 2, 1 - index, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        }
        for (const index of [1, 0]) {
            const gl = contexts[index];
            gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
            if (gl.getError() !== gl.NO_ERROR ||
                pixel[0] !== index * 255 || pixel[1] !== (iteration % 2) * 255 ||
                pixel[2] !== (1 - index) * 255 || pixel[3] !== 255) {
                throw new Error(`Incorrect readback in context ${index}, iteration ${iteration}: ${pixel}`);
            }
        }
    }
    return '128 cross-context pixel readbacks matched';
}
"""


class Host:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        self.next_id = 1
        # The host's diagnostics, shown when a step fails.
        self.stderr = []
        threading.Thread(target=self._drain_stderr, daemon=True).start()

    def _drain_stderr(self):
        for line in self.process.stderr:
            self.stderr.append(line.decode("utf-8", "replace").rstrip())

    def send(self, message):
        body = json.dumps(message).encode()
        self.process.stdin.write(struct.pack("<I", len(body)) + body)
        self.process.stdin.flush()

    def receive(self):
        head = self.process.stdout.read(4)
        if len(head) < 4:
            raise RuntimeError("the host closed its output")
        (length,) = struct.unpack("<I", head)
        return json.loads(self.process.stdout.read(length))

    def call(self, op, **fields):
        request_id = self.next_id
        self.next_id += 1
        self.send({"id": request_id, "browser": {"op": op, **fields}})
        answer = self.receive()
        if answer.get("id") != request_id:
            raise RuntimeError(f"{op}: answer to another request: {answer}")
        result = answer["browser"]
        if "error" in result:
            raise RuntimeError(f"{op}: {result['error']}")
        return result["ok"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    name = "ABHost.exe" if os.name == "nt" else "ABHost"
    host = Host(os.path.join(sys.argv[1], name))
    watchdog = threading.Timer(TIMEOUT, host.process.kill)
    watchdog.start()
    try:
        host.send({"hello": {}})
        welcome = host.receive()
        welcome = welcome["welcome"]
        build = welcome.get("build", {})
        print(f"welcome: {welcome['ops']}, revision {welcome.get('revision', 0)}, "
              f"{build.get('commit')} built {build.get('date')}")

        block = host.call("open", url="about:blank", width=640, height=480)["block"]
        host.call("goto", block=block, url=PAGE)
        after = 0
        deadline = time.monotonic() + TIMEOUT
        while True:
            answer = host.call("events", block=block, after=after, wait=True)
            after = answer["next"]
            kinds = [event["type"] for event in answer["events"]]
            if "failed" in kinds:
                raise RuntimeError(f"load failed: {answer['events']}")
            if any(event["type"] == "loaded" and event.get("url", "").startswith("data:") for event in answer["events"]):
                break
            if time.monotonic() > deadline:
                raise RuntimeError("the page did not load")
        print("loaded")

        value = host.call("eval", block=block, function="() => document.title + ' ' + document.getElementById('x').textContent")
        if value.get("value") != "smoke hello":
            raise RuntimeError(f"eval: {value}")
        print(f"eval: {value['value']}")

        # Before the readback, which replaces the page's content.
        width = IMAGE_SIZE * len(IMAGE_COLORS)
        shot = host.call("capture", block=block, format="png", rect={"x": 0, "y": 0, "width": width, "height": IMAGE_SIZE})
        row = png_rows(base64.b64decode(shot["data"]))[IMAGE_SIZE // 2]
        painted = [row[IMAGE_SIZE * index + IMAGE_SIZE // 2] for index in range(len(IMAGE_COLORS))]
        if any(max(abs(a - b) for a, b in zip(seen, color)) > 16 for seen, color in zip(painted, IMAGE_COLORS)):
            raise RuntimeError(f"images: painted {painted}, expected {IMAGE_COLORS}")
        print(f"images: {len(painted)} painted")

        if sys.platform == "darwin":
            readback = host.call("eval", block=block, function=READBACK)
            if readback.get("value") != "128 cross-context pixel readbacks matched":
                raise RuntimeError(f"readback: {readback}")
            print(f"readback: {readback['value']}")

        shot = host.call("capture", block=block, format="png")
        if shot["mime"] != "image/png" or not shot["data"] or shot["width"] != 640:
            raise RuntimeError(f"capture: {dict(shot, data=len(shot.get('data', '')))}")
        print(f"capture: {shot['width']}x{shot['height']}, {len(shot['data'])} base64 bytes")

        host.call("close", block=block)
        host.process.stdin.close()
        status = host.process.wait(timeout=TIMEOUT)
        if status != 0:
            raise RuntimeError(f"the host exited with status {status}")
        print("exited: 0")
    except Exception as error:
        print(f"FAILED: {error}", file=sys.stderr)
        host.process.kill()
        host.process.wait()
        print("\n".join(host.stderr[-50:]), file=sys.stderr)
        sys.exit(1)
    finally:
        watchdog.cancel()


if __name__ == "__main__":
    main()
