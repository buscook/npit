import fcntl
import os
import pty
import select
import signal
import subprocess
import struct
import tempfile
import termios
import time
import unittest
import wave
from pathlib import Path


APP = Path(__file__).resolve().parents[1] / "npit"


def audio(path, seconds=3):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(8000)
        output.writeframes(b"\0\0" * (8000 * seconds))


class Terminal:
    def __init__(self, *arguments, art_mode="none", env=None):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        self.process = subprocess.Popen(
            [str(APP), "--no-visualizer", "--no-lyrics", "--art-mode", art_mode, *map(str, arguments)],
            stdin=slave, stdout=slave, stderr=slave,
            preexec_fn=os.setsid,
            env={**os.environ, "TERM": "xterm-256color", **(env or {})},
        )
        os.close(slave)
        self.output = bytearray()

    def wait_for(self, value, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if value in self.output.decode("utf-8", "replace"):
                return
            ready, _, _ = select.select([self.master], [], [], 0.2)
            if ready:
                try:
                    self.output.extend(os.read(self.master, 65536))
                except OSError:
                    break
        raise AssertionError(f"Expected {value!r} in terminal output: {self.output[-3000:]!r}")

    def send(self, value):
        os.write(self.master, value.encode())

    def read_for(self, seconds):
        start = len(self.output)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], min(0.1, deadline - time.monotonic()))
            if ready:
                try:
                    self.output.extend(os.read(self.master, 65536))
                except OSError:
                    break
        return bytes(self.output[start:])

    def resize(self, rows, columns):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.killpg(self.process.pid, signal.SIGWINCH)

    def close(self):
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
        self.process.wait(timeout=5)
        os.close(self.master)


class LocalPlaybackTests(unittest.TestCase):
    def test_folder_uses_directory_order_and_track_number(self):
        with tempfile.TemporaryDirectory() as location:
            folder = Path(location)
            audio(folder / "99-last.wav", 1)
            audio(folder / "01-first.wav", 1)
            with os.scandir(folder) as items:
                names = [Path(entry.name).stem for entry in items]
            terminal = Terminal(folder)
            try:
                terminal.wait_for(names[0])
                terminal.wait_for("track 1")
                terminal.wait_for(names[1], 12)
                terminal.wait_for("track 2")
            finally:
                terminal.close()

    def test_metadata_preferred_and_filename_fallback(self):
        with tempfile.TemporaryDirectory() as location:
            folder = Path(location)
            source = Path(location) / "source.wav"
            audio(source)
            tagged_folder = folder / "tagged"
            tagged_folder.mkdir()
            tagged = tagged_folder / "file-name.flac"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(source), "-metadata", "title=Tagged Title", "-metadata", "artist=Tagged Artist", str(tagged)], check=True)
            terminal = Terminal(tagged_folder)
            try:
                terminal.wait_for("Tagged Title")
                terminal.wait_for("Tagged Artist")
            finally:
                terminal.close()
            plain_folder = folder / "plain"
            plain_folder.mkdir()
            plain = plain_folder / "source.wav"
            source.rename(plain)
            terminal = Terminal(plain_folder)
            try:
                terminal.wait_for("source")
                terminal.wait_for("track 1")
            finally:
                terminal.close()

    def test_missing_dropped_file_reports_error(self):
        with tempfile.TemporaryDirectory() as location:
            terminal = Terminal()
            try:
                terminal.wait_for("no media playing")
                terminal.send(str(Path(location) / "missing.wav") + "\n")
                terminal.wait_for("cannot open media:")
            finally:
                terminal.close()

    def test_seek_controls(self):
        with tempfile.TemporaryDirectory() as location:
            folder = Path(location)
            media_folder = folder / "media"
            media_folder.mkdir()
            media = media_folder / "seek.wav"
            audio(media, 20)
            config = folder / "settings.toml"
            config.write_text("[controls]\nenabled = true\n")
            terminal = Terminal("--config", config, media_folder)
            try:
                terminal.wait_for("seek")
                terminal.send("]")
                terminal.wait_for("0:05", 10)
                terminal.send("[")
                terminal.wait_for("0:00", 10)
            finally:
                terminal.close()

    def test_empty_folder_does_not_replace_playing_song(self):
        with tempfile.TemporaryDirectory() as location:
            folder = Path(location)
            playing = folder / "playing"
            playing.mkdir()
            audio(playing / "keep.wav", 12)
            empty = folder / "empty"
            empty.mkdir()
            terminal = Terminal(playing)
            try:
                terminal.wait_for("keep")
                terminal.send(str(empty) + "\n")
                terminal.wait_for("no playable media found")
                terminal.resize(25, 101)
                following = terminal.read_for(0.7)
                self.assertNotIn(b"no media playing", following)
                self.assertIn(b"keep", following)
            finally:
                terminal.close()

    def test_video_redraws_after_resize(self):
        with tempfile.TemporaryDirectory() as location:
            folder = Path(location)
            video = folder / "clip.mp4"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=160x90:r=10", "-t", "4", "-c:v", "mpeg4", str(video)], check=True)
            terminal = Terminal(folder, art_mode="color")
            try:
                terminal.wait_for("clip")
                terminal.read_for(0.8)
                steady = terminal.read_for(0.5)
                terminal.resize(30, 120)
                resized = terminal.read_for(0.7)
                self.assertIn(b"\x1b[2J", resized)
                self.assertIn(b"clip", resized)
                self.assertGreater(len(resized), len(steady))
            finally:
                terminal.close()

    def test_slow_player_query_does_not_block_display(self):
        with tempfile.TemporaryDirectory() as location:
            playerctl = Path(location) / "playerctl"
            playerctl.write_text("#!/bin/sh\nsleep 2\nexit 1\n")
            playerctl.chmod(0o755)
            terminal = Terminal(env={"PATH": f"{location}:{os.environ['PATH']}", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent"})
            started = time.monotonic()
            try:
                terminal.wait_for("no media playing", 1.5)
                self.assertLess(time.monotonic() - started, 1.5)
            finally:
                terminal.close()


if __name__ == "__main__":
    unittest.main()
