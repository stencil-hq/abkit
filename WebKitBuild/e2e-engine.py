#!/usr/bin/env python3
"""Headless macOS ABHost end-to-end tests (stdlib Python plus the system clang).

Usage: e2e-engine.py ENGINE_DIR [TEST...]

Tests: welcome keys clipboard-private clipboard-system context-menu live inspect
       inspector-menu

No network, GUI windows, or input to the user's desktop. The system clipboard test
is skipped unless ABKIT_E2E_CLIPBOARD=1; it restores the previous plain text, not
other pasteboard formats. All other tests leave the system pasteboard untouched.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import quote

TIMEOUT = 15
DEADLINE = float("inf")
POLL = threading.Event()
PAGE = """<!doctype html><title>ABKit e2e</title>
<style>body { margin: 0; font: 18px sans-serif; } input, a, #prevent {
position: absolute; left: 20px; width: 320px; height: 36px; }
input { top: 20px; } a { top: 100px; } #prevent { top: 160px; }</style>
<input id=field value="one two three"><a href="https://example.invalid/e2e">link</a>
<div id=prevent>custom context menu</div><p style="margin-top:220px">Inspect this DOM</p>
<script>
window.contextHits = [];
document.addEventListener('contextmenu', event => {
    contextHits.push(event.target.id || event.target.tagName);
    if (event.target.id === 'prevent') event.preventDefault();
});
</script>"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def remaining(timeout=TIMEOUT):
    value = min(timeout, DEADLINE - time.monotonic())
    require(value > 0, "suite exceeded its 110-second deadline")
    return value


def eventually(probe, description, timeout=TIMEOUT):
    deadline = time.monotonic() + remaining(timeout)
    last = None
    while time.monotonic() < deadline:
        last = probe()
        if last:
            return last
        POLL.wait(min(0.05, max(0, deadline - time.monotonic())))
    raise RuntimeError(f"timed out waiting for {description}; last result: {last!r}")


class Host:
    def __init__(self, executable, system=False):
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        self.next_id = 1
        self.answers = {}
        self.condition = threading.Condition()
        self.reader_error = None
        self.stderr = []
        self.after = {}
        self.reader = threading.Thread(target=self._read_answers, daemon=True)
        self.diagnostics = threading.Thread(target=self._drain_stderr, daemon=True)
        self.reader.start()
        self.diagnostics.start()
        try:
            self.send({"hello": {"clipboard": "system"} if system else {}})
            self.ops = self.answer("welcome")["welcome"]["ops"]
        except Exception:
            self.__exit__(*sys.exc_info())
            raise

    def _drain_stderr(self):
        for line in self.process.stderr:
            self.stderr.append(line.decode("utf-8", "replace").rstrip())
            self.stderr[:] = self.stderr[-80:]

    def _read_exact(self, length):
        data = bytearray()
        while len(data) < length:
            part = self.process.stdout.read(length - len(data))
            if not part:
                raise RuntimeError("the host closed its output")
            data.extend(part)
        return data

    def _read_answers(self):
        try:
            while True:
                length, = struct.unpack("<I", self._read_exact(4))
                require(length <= 256 * 1024 * 1024, f"invalid frame length: {length}")
                message = json.loads(self._read_exact(length))
                with self.condition:
                    self.answers[message.get("id", "welcome")] = message
                    self.condition.notify_all()
        except Exception as error:
            with self.condition:
                self.reader_error = error
                self.condition.notify_all()

    def send(self, message):
        body = json.dumps(message).encode()
        self.process.stdin.write(struct.pack("<I", len(body)) + body)
        self.process.stdin.flush()

    def request(self, op, **fields):
        request_id = self.next_id
        self.next_id += 1
        self.send({"id": request_id, "browser": {"op": op, **fields}})
        return request_id

    def answer(self, request_id, timeout=TIMEOUT):
        with self.condition:
            ready = self.condition.wait_for(
                lambda: request_id in self.answers or self.reader_error, remaining(timeout)
            )
            require(ready, f"request {request_id}: no answer within {timeout}s")
            if request_id not in self.answers:
                raise self.reader_error
            return self.answers.pop(request_id)

    def call(self, op, timeout=TIMEOUT, error=None, **fields):
        result = self.answer(self.request(op, **fields), timeout)["browser"]
        if error:
            require(result.get("error", {}).get("kind") == error,
                    f"{op}: expected {error}, got {result}")
            return result["error"]
        require("ok" in result, f"{op}: {result}")
        return result["ok"]

    def evaluate(self, block, function, **fields):
        return self.call("eval", block=block, function=function, **fields).get("value")

    def page(self):
        block = self.call("open", url="about:blank", width=800, height=600)["block"]
        self.call("goto", block=block, url="data:text/html," + quote(PAGE))
        deadline = time.monotonic() + remaining()
        while time.monotonic() < deadline:
            events = self.events(block, wait=True, timeout=max(0.01, deadline - time.monotonic()))
            require(not any(event["type"] == "failed" for event in events), f"load failed: {events}")
            if any(event["type"] == "loaded" and event.get("url", "").startswith("data:") for event in events):
                return block
        raise RuntimeError("the page did not load")

    def events(self, block, **fields):
        result = self.call("events", block=block, after=self.after.get(block, 0), **fields)
        require(not result["dropped"], f"events were dropped: {result}")
        self.after[block] = result["next"]
        return result["events"]

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        try:
            self.process.stdin.close()
            status = self.process.wait(timeout=5)
            if kind is None:
                require(status == 0, f"host exited with status {status}")
        except Exception:
            self.process.kill()
            self.process.wait()
            if kind is None:
                raise
        finally:
            self.reader.join(timeout=1)
            self.diagnostics.join(timeout=1)
            if kind:
                print("\n".join(self.stderr[-50:]), file=sys.stderr)
            self.process.stdout.close()
            self.process.stderr.close()


def key(name, *mods):
    return [{"type": "key", "action": action, "key": name, "mods": list(mods)}
            for action in ("down", "up")]


def press(host, block, name, *mods):
    require(host.call("input", block=block, events=key(name, *mods)) == {}, "input did not answer {}")


def field(host, block, text="one two three", start=None, end=None):
    start = len(text) if start is None else start
    end = start if end is None else end
    host.evaluate(block, "(text, start, end) => { const f = document.getElementById('field'); "
                  "f.focus(); f.value = text; f.setSelectionRange(start, end); }",
                  args=[text, start, end])


def field_state(host, block):
    return host.evaluate(block, "() => { const f = document.getElementById('field'); "
                         "return [f.value, f.selectionStart, f.selectionEnd]; }")


def right_click(host, block, x, y, live=False):
    steps = [{"type": "mouse", "action": "move", "x": x, "y": y}]
    steps.extend({"type": "mouse", "action": action, "button": "right", "x": x, "y": y}
                 for action in ("down", "up"))
    require(host.call("input", block=block, events=steps, live=live) == {}, "mouse input did not answer {}")


def test_welcome(executable, helper):
    for system in (False, True):
        with Host(executable, system) as host:
            require({"contextMenu", "live", "inspect", "present"}.issubset(host.ops), f"missing ops: {host.ops}")
            require(("systemClipboard" in host.ops) == system, f"wrong clipboard negotiation: {host.ops}")
    return "contextMenu live inspect present; systemClipboard only when requested"


def test_keys(executable, helper):
    with Host(executable) as host:
        block = host.page()
        field(host, block, start=0, end=3)
        press(host, block, "c", "meta")
        require(field_state(host, block) == ["one two three", 0, 3], "Command-C typed or changed the selection")
        require(host.call("clipboard")["text"] == "one", "Command-C did not copy the selection")
        field(host, block)
        press(host, block, "ArrowLeft", "alt")
        require(field_state(host, block) == ["one two three", 8, 8], "Option-Left did not move by word")
        press(host, block, "ArrowLeft", "meta")
        require(field_state(host, block) == ["one two three", 0, 0], "Command-Left did not move to line start")
        press(host, block, "a", "meta")
        require(field_state(host, block) == ["one two three", 0, 13], "Command-A did not select all")
        press(host, block, "ArrowRight")
        press(host, block, "Backspace", "alt")
        require(field_state(host, block) == ["one two ", 8, 8], "Option-Backspace did not delete a word")
        press(host, block, "z", "meta")
        require(field_state(host, block)[0] == "one two three", "Command-Z did not undo deletion")
        field(host, block, "")
        press(host, block, "™", "alt")
        require(field_state(host, block) == ["™", 1, 1], "Option-typed character was not inserted")
    return "Command-C/A/Left/Z, Option-Left/Backspace, and Option-typed ™"


def pasteboard():
    return subprocess.run(["/usr/bin/pbpaste"], check=True, stdout=subprocess.PIPE, timeout=5).stdout


def copy_to_pasteboard(text):
    subprocess.run(["/usr/bin/pbcopy"], input=text, check=True, timeout=5)


def test_clipboard_private(executable, helper):
    before = pasteboard()
    with Host(executable) as host:
        block = host.page()
        field(host, block, "ABKit private clipboard", 0, 23)
        press(host, block, "c", "meta")
        require(host.call("clipboard")["text"] == "ABKit private clipboard", "private copy failed")
        require(pasteboard() == before, "private copy changed the system pasteboard")
    require(pasteboard() == before, "host exit changed the system pasteboard")
    return "Command-C stays private; pbpaste unchanged (read-only)"


def test_clipboard_system(executable, helper):
    if os.environ.get("ABKIT_E2E_CLIPBOARD") != "1":
        return "SKIP (set ABKIT_E2E_CLIPBOARD=1; overwrites pasteboard, restores plain text only)"
    before = pasteboard()
    try:
        with Host(executable, system=True) as host:
            block = host.page()
            copied = "ABKit system copy " + str(os.getpid())
            field(host, block, copied, 0, len(copied))
            press(host, block, "c", "meta")
            eventually(lambda: pasteboard() == copied.encode(), "Command-C on system pasteboard")
            pasted = "ABKit external paste ™"
            copy_to_pasteboard(pasted.encode())
            field(host, block, "")
            press(host, block, "v", "meta")
            eventually(lambda: field_state(host, block)[0] == pasted, "Command-V from system pasteboard")
    finally:
        copy_to_pasteboard(before)
    require(pasteboard() == before, "plain-text pasteboard restoration failed")
    return "Command-C reaches pbpaste, Command-V reads pbcopy; previous plain text restored"


def test_context_menu(executable, helper):
    with Host(executable) as host:
        block = host.page()
        for x, y, expected in ((40, 115, "link"), (40, 35, "editable")):
            host.events(block)
            right_click(host, block, x, y)
            def menu():
                return next((event for event in host.events(block) if event["type"] == "contextMenu"), None)
            event = eventually(menu, f"{expected} contextMenu")
            require(event.get("x") == x and event.get("y") == y, f"wrong contextMenu coordinates: {event}")
            if expected == "link":
                require(event.get("link") == "https://example.invalid/e2e", f"missing link: {event}")
                require(event.get("editable") is False, f"link reported editable: {event}")
            else:
                require(event.get("editable") is True, f"input not editable: {event}")
        host.events(block)
        right_click(host, block, 40, 175)
        eventually(lambda: host.evaluate(block, "() => contextHits.includes('prevent')"), "preventDefault listener")
        # An input round trip and two animation frames ensure the context menu request,
        # had there been one, has reached the host. No fixed negative-event sleep.
        host.call("input", block=block, events=[])
        events = host.events(block)
        require(not any(event["type"] == "contextMenu" for event in events), f"preventDefault leaked a menu: {events}")
    return "link/editable coordinates; preventDefault suppresses event"


def test_live(executable, helper):
    with Host(executable) as host:
        block = host.page()
        field(host, block, "")
        # Hold the page's JS thread (and thus all animation frames). The console
        # marker confirms the hold began; a normal input round trip cannot finish
        # during it, whereas live input must acknowledge dispatch immediately.
        pending = host.request("eval", block=block, function="() => { console.log('ABKit e2e busy'); "
                               "const end = Date.now() + 1500; while (Date.now() < end) {} return true; }")
        eventually(lambda: any(event["type"] == "console" and event.get("text") == "ABKit e2e busy"
                               for event in host.events(block)), "page busy marker")
        started = time.monotonic()
        events = [step for character in "abc" for step in key(character)]
        require(host.call("input", block=block, events=events, live=True, timeout=0.75) == {}, "live answer was not {}")
        elapsed = time.monotonic() - started
        with host.condition:
            require(pending not in host.answers, "live input waited for the blocked page / animation frames")
        require(host.answer(pending)["browser"].get("ok", {}).get("value") is True, "busy script failed")
        eventually(lambda: field_state(host, block) == ["abc", 3, 3], "ordered live key delivery")
    return f"abc in order; dispatch acknowledged in {elapsed * 1000:.1f}ms while page/frames blocked"


class Presentation:
    def __init__(self, helper):
        self.service = f"so.stencil.abkit.e2e.{os.getpid()}.{time.monotonic_ns()}"
        self.process = subprocess.Popen([helper, self.service], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.condition = threading.Condition()
        self.messages = []
        self.error = None
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            for line in self.process.stdout:
                message = json.loads(line)
                with self.condition:
                    self.messages.append(message)
                    self.condition.notify_all()
            raise RuntimeError("presentation helper closed: " + self.process.stderr.read())
        except Exception as error:
            with self.condition:
                self.error = error
                self.condition.notify_all()

    def wait(self, predicate, description):
        with self.condition:
            ready = self.condition.wait_for(
                lambda: self.error or any(predicate(message) for message in self.messages), remaining()
            )
            if self.error:
                raise self.error
            require(ready, f"no {description}; presentation messages: {self.messages[-5:]}")
            return next(message for message in reversed(self.messages) if predicate(message))

    def __enter__(self):
        try:
            self.wait(lambda message: message.get("ready"), "Mach service readiness")
        except Exception:
            self.__exit__(*sys.exc_info())
            raise
        return self

    def __exit__(self, *exception):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.reader.join(timeout=1)
        self.process.stdout.close()
        self.process.stderr.close()


def present_inspector(host, block, receiver):
    inspector = host.call("inspect", block=block)["block"]
    require(inspector != block, "inspect returned the inspected block itself")
    require(host.call("inspect", block=block) == {"block": inspector}, "inspect is not idempotent")
    require(host.call("viewport", block=inspector, width=1100, height=720, scale=2) == {}, "viewport answer")
    require(host.call("present", block=inspector, to={"mach": receiver.service}) == {}, "present answer")
    require(host.call("focus", block=inspector, value=True) == {}, "focus answer")
    receiver.wait(lambda message: message.get("hello"), "Mach hello")
    frame = receiver.wait(lambda message: message.get("block") == inspector
                          and message.get("width") == 2200 and message.get("height") == 1440
                          and message.get("variance", 0) > 20, "2200x1440 non-flat inspector frame")
    return inspector, frame


def test_inspect(executable, helper):
    with Presentation(helper) as receiver, Host(executable) as host:
        block = host.page()
        inspector, frame = present_inspector(host, block, receiver)
        # Every page-only operation must be rejected before validating its fields.
        unsupported = ("state goto nav dialogs dialog scripts eval edit capture cookies setCookie "
                       "deleteCookie agent appearance allow insecure fileAccess credentials downloads "
                       "files dom identity inspect pdf media").split()
        for op in unsupported:
            host.call(op, block=inspector, error="unsupported")
        host.call("close", block=inspector)
        def block_ids():
            return {entry["block"] for entry in host.call("blocks")["blocks"]}
        eventually(lambda: inspector not in block_ids(), "closed inspector removal")
        replacement = host.call("inspect", block=block)["block"]
        require(replacement != inspector and replacement != block, "reopened inspector did not get a new block")
        host.call("close", block=block)
        eventually(lambda: not block_ids(), "inspected page and inspector removal")
    return f"idempotent; 2200x1440 variance={frame['variance']:.1f}; {len(unsupported)} unsupported ops; close/reopen/cascade"


def test_inspector_menu(executable, helper):
    with Presentation(helper) as receiver, Host(executable) as host:
        block = host.page()
        inspector, _ = present_inspector(host, block, receiver)
        def menu():
            right_click(host, inspector, 60, 90, live=True)
            return next((event for event in host.events(inspector)
                         if event["type"] == "contextMenu" and event.get("items")), None)
        event = eventually(menu, "inspector DOM-tree context menu")
        require(event.get("x") == 60 and event.get("y") == 90, f"wrong inspector menu position: {event}")
        choices = []
        def validate(items):
            require(isinstance(items, list) and items, f"invalid menu items: {items}")
            for item in items:
                if item.get("separator") is True:
                    continue
                require(all(name in item for name in ("id", "label", "enabled", "checked", "checkable")),
                        f"incomplete menu item: {item}")
                require(isinstance(item["id"], int) and isinstance(item["label"], str), f"invalid menu item: {item}")
                require(all(isinstance(item[name], bool) for name in ("enabled", "checked", "checkable")),
                        f"invalid menu flags: {item}")
                if "submenu" in item:
                    validate(item["submenu"])
                elif item["enabled"]:
                    choices.append(item)
        validate(event["items"])
        require(choices, f"menu has no enabled leaf item: {event}")
        # Prefer a harmless view action over copy (even though this host is private).
        choice = next((item for item in choices if "Expand" in item["label"]), choices[0])
        require(host.call("menu", block=inspector, item=choice["id"]) == {}, "menu did not answer {}")
    return f"DOM row reports structured items; chose {choice['label']!r} -> {{}}"


TESTS = {
    "welcome": test_welcome,
    "keys": test_keys,
    "clipboard-private": test_clipboard_private,
    "clipboard-system": test_clipboard_system,
    "context-menu": test_context_menu,
    "live": test_live,
    "inspect": test_inspect,
    "inspector-menu": test_inspector_menu,
}


def main():
    global DEADLINE
    if len(sys.argv) < 2 or any(name not in TESTS for name in sys.argv[2:]):
        print(__doc__, file=sys.stderr)
        return 2
    if sys.platform != "darwin":
        print("SKIP: e2e-engine.py requires macOS")
        return 0
    started = time.monotonic()
    DEADLINE = started + 110
    names = sys.argv[2:] or list(TESTS)
    executable = str(Path(sys.argv[1]).resolve() / "ABHost")
    name = "setup"
    try:
        require(os.access(executable, os.X_OK), f"not an executable: {executable}")
        with tempfile.TemporaryDirectory(prefix="abkit-e2e-") as directory:
            helper = str(Path(directory) / "present")
            if any(test in names for test in ("inspect", "inspector-menu")):
                source = Path(__file__).resolve().parent / "e2e" / "present.c"
                subprocess.run(["/usr/bin/xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra",
                                str(source), "-framework", "CoreFoundation", "-framework", "IOSurface",
                                "-o", helper], check=True, timeout=remaining(30))
            for name in names:
                print(f"{name}: {TESTS[name](executable, helper)}", flush=True)
        print(f"PASS: {len(names)} tests, {time.monotonic() - started:.2f}s (including skipped tests)")
        return 0
    except Exception as error:
        print(f"FAILED {name}: {error}", file=sys.stderr)
        print(f"elapsed: {time.monotonic() - started:.2f}s", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
