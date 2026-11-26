import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run_behavior_test(stubs, harness, headers):
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        (temp / "stubs.h").write_text(stubs)
        for header in headers:
            path = temp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "stubs.h"\n')
        source = temp / "test.c"
        source.write_text(harness)
        executable = temp / "test"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Werror=implicit-function-declaration",
                f"-I{temp}",
                f"-I{ROOT / 'src'}",
                str(source),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)
