"""Exercise actual supervisor lifetime with a stub power API, not firmware."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="bootstrap-", dir=binary.parent) as directory:
    folder = Path(directory)
    fixture = folder / "fixture"
    fixture.mkdir()
    script = fixture / "install.sh"
    log = fixture / "power.log"
    for code in (0, 7):
        script.write_text(f"exit {code}\n")
        log.unlink(missing_ok=True)
        assert subprocess.run([binary], cwd=folder).returncode == code
        assert log.read_text().splitlines() == ["acquire", "release"]
    # Missing script is an exec/script failure, but must release protection.
    script.unlink()
    log.unlink()
    assert subprocess.run([binary], cwd=folder).returncode != 0
    assert log.read_text().splitlines() == ["acquire", "release"]
    # No work or power assertion on subsequent boot.
    (fixture / "complete").touch()
    log.unlink()
    assert subprocess.run([binary], cwd=folder).returncode == 0
    assert not log.exists()
    (fixture / "complete").unlink()
    script.write_text("echo started > fixture/started\nexit 0\n")
    assert subprocess.run([binary], cwd=folder, env={**os.environ, "S5LBOX_TEST_POWER_FAIL": "1"}).returncode == 1
    assert not (fixture / "started").exists()
    assert log.read_text().splitlines() == ["acquire"]
    log.unlink()
    # Termination is forwarded; the assertion stays alive until the child ends.
    script.write_text("echo started > fixture/started\nexec sleep 30\n")
    process = subprocess.Popen([binary], cwd=folder)
    try:
        deadline = time.monotonic() + 5
        while not (fixture / "started").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert (fixture / "started").exists()
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=5) == 128 + signal.SIGTERM
        assert log.read_text().splitlines() == ["acquire", "release"]
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
print("Guest bootstrap supervisor: lifecycle checks passed")
