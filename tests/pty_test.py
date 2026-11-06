#!/usr/bin/env python3
"""PTY integration test of build/demo: startup, keys split across reads,
resize, cleanup on exit and on SIGTERM, terminal mode restoration."""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import termios
import time


def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


class Demo:
    def __init__(self, term, args=(), program="demo", rows=14, cols=68):
        self.master, self.slave = pty.openpty()
        set_size(self.slave, rows, cols)
        self.modes = termios.tcgetattr(self.slave)
        self.data = bytearray()
        self.pid = os.fork()
        if self.pid == 0:
            os.close(self.master)
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
            for fd in (0, 1, 2):
                os.dup2(self.slave, fd)
            os.environ["TERM"] = term
            os.environ.pop("COLORTERM", None)
            os.environ.pop("NO_COLOR", None)
            os.execv("./build/" + program, [program, *args])

    def screen(self):
        """Renders the output: cursor positioning and printable characters."""
        grid, x, y = {}, 0, 0
        text = self.data.decode("utf-8", "replace")
        for tok in re.findall(r"\x1b\[[0-?]*[ -/]*[@-~]|\x1b[()][0-2A-Z]|\x1b[=>]|[^\x1b]", text):
            if tok.startswith("\x1b[") and tok.endswith("H"):
                nums = (tok[2:-1].split(";") + ["1"])[:2]
                y, x = int(nums[0] or 1) - 1, int(nums[1] or 1) - 1
            elif tok in ("\x1b[J", "\x1b[2J"):
                grid = {}
            elif not tok.startswith("\x1b") and ord(tok) >= 32:
                grid[x, y] = tok
                x += 1
        return "\n".join("".join(grid.get((x, y), " ") for x in range(100)) for y in range(30))

    def until(self, needle, on_screen=False, timeout=4):
        deadline = time.monotonic() + timeout
        while needle not in (self.screen() if on_screen else self.data):
            if time.monotonic() > deadline:
                raise AssertionError((needle, bytes(self.data[-300:])))
            if select.select([self.master], [], [], 0.05)[0]:
                self.data.extend(os.read(self.master, 65536))

    def send(self, *parts):
        """Writes parts with a pause between them, as separate reads."""
        for part in parts:
            os.write(self.master, part)
            time.sleep(0.01)

    def finish(self):
        self.until(b"\x1b[?1049l")
        for _ in range(200):
            done, status = os.waitpid(self.pid, os.WNOHANG)
            if done:
                assert os.waitstatus_to_exitcode(status) == 0
                break
            time.sleep(0.01)
        else:
            raise AssertionError("demo did not exit")
        assert termios.tcgetattr(self.slave) == self.modes, "terminal modes not restored"
        self.pid = None

    def close(self):
        if self.pid:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        os.close(self.master)
        os.close(self.slave)


def session(term, info, args=()):
    demo = Demo(term, args)
    try:
        demo.until("Latin: ą ć ę".encode())
        demo.until(info, on_screen=True)
        assert not termios.tcgetattr(demo.slave)[3] & termios.ICANON
        demo.send(b"\x1b[1;", b"5D")
        demo.until("key=260 rune=U+0000 mods=2", on_screen=True)  # Ctrl-Left
        demo.send(b"\x1b[[", b"A")
        demo.until("key=279 rune=U+0000 mods=0", on_screen=True)  # Linux F1
        demo.send(b"\x1bO", b"P")
        demo.until("key=279 rune=U+0000 mods=0", on_screen=True)  # SS3 F1
        demo.send("ą".encode()[:1], "ą".encode()[1:])
        demo.until("rune=U+0105", on_screen=True)
        set_size(demo.slave, 16, 72)
        demo.until(b"\x1b[16;1H")
        demo.send(b"q")
        demo.finish()
    finally:
        demo.close()


def terminated(term):
    demo = Demo(term)
    try:
        demo.until("Latin: ą ć ę".encode())
        os.kill(demo.pid, signal.SIGTERM)
        demo.finish()
    finally:
        demo.close()


def tetris():
    game = Demo("xterm-256color", program="tetris", rows=24, cols=80)
    try:
        game.until("TETRIS", on_screen=True)
        game.until("LEVEL <  0 >", on_screen=True)
        game.send(b"\x1b[C", b"\x1b[C")
        game.until("LEVEL <  2 >", on_screen=True)
        game.send(b"\r")
        game.until("NEXT", on_screen=True)
        game.until("STATISTICS", on_screen=True)
        time.sleep(0.4)  # the first piece spawns after its entry delay
        game.send(b"\x1b[D", b"x", b"\x1b[B")
        game.send(b"p")
        game.until("PAUSE", on_screen=True)
        game.send(b"q")
        game.finish()
    finally:
        game.close()


session("xterm-256color", "xterm-256color, 256 colors")
session("any-terminal", "any-terminal, 256 colors")
session("linux", "linux, 8 colors")
session("unknown", "xterm, 8 colors", ["--term", "xterm"])
terminated("xterm-256color")
tetris()
print("PTY: xterm-256color, any-terminal, linux, --term; split keys, resize, exit, SIGTERM;"
      " tetris passed")
