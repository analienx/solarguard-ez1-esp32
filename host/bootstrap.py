from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import venv
from pathlib import Path

from .errors import UnsupportedPython


ROOT = Path(__file__).resolve().parents[1]


def ensure_runtime() -> None:
    if sys.version_info < (3, 10):
        raise UnsupportedPython("Python 3.10 or newer is required")
    if os.environ.get("EZ1_WATCHDOG_SKIP_BOOTSTRAP") == "1":
        return
    environment = ROOT / ".venv"
    python = environment / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    lock = ROOT / "requirements.lock"
    stamp = environment / ".requirements.sha256"
    digest = hashlib.sha256(lock.read_bytes()).hexdigest()
    inside_environment = Path(sys.prefix).resolve() == environment.resolve()
    if not python.exists():
        print("Creating local Python environment…")
        venv.EnvBuilder(with_pip=True).create(environment)
    if not stamp.exists() or stamp.read_text(encoding="ascii").strip() != digest:
        print("Installing pinned commissioning tools…")
        subprocess.run(
            [str(python), "-m", "pip", "install", "--requirement", str(lock)],
            check=True,
        )
        stamp.write_text(digest + "\n", encoding="ascii")
    if not inside_environment:
        os.execv(str(python), [str(python), str(ROOT / "watchdog.py"), *sys.argv[1:]])