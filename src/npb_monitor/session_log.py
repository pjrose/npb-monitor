from __future__ import annotations

import json
import threading
from pathlib import Path

from .state import MonitorState


class SessionLogger:
    def __init__(self, state: MonitorState, project_root: Path, interval_s: float = 30.0) -> None:
        self.state = state
        self.log_dir = project_root / "logs"
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self._thread = threading.Thread(target=self._run, name="npb-session-log", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
        self.write_current()
        self.append_final()

    def write_current(self) -> None:
        summary = self.state.session_summary()
        target = self.log_dir / "current_session.json"
        temp = self.log_dir / "current_session.tmp"
        temp.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        temp.replace(target)

    def append_final(self) -> None:
        summary = self.state.session_summary()
        target = self.log_dir / "sessions.jsonl"
        with target.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(summary, separators=(",", ":")) + "\n")

    def _run(self) -> None:
        while not self._stop.wait(self.interval_s):
            self.write_current()
