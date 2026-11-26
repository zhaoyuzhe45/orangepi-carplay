#!/usr/bin/env python3
"""Offline regression using the real native vehicle implementation."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/legacy-services-20261111"
OUT.mkdir(parents=True, exist_ok=True)
cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
binary = OUT / ("vehicle_services_test.exe" if os.name == "nt" else "vehicle_services_test")
subprocess.run([cc, "-D__USE_MINGW_ANSI_STDIO=1", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "src"), str(ROOT / "tests/vehicle_services_test.c"),
                str(ROOT / "src/carplay_vehicle_state.c"), "-pthread", "-static", "-o", str(binary)], check=True)
raise SystemExit(subprocess.run([str(binary)], timeout=10).returncode)
