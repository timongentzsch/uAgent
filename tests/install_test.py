"""install.sh against a fake Docker and a fake download: what a new user is
told, and what is written, for each state Docker and the port can be in. The
script runs as a release install from a copy, so nothing is built."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = "9.9.9"
PORT = "8080"

# Logs each call; FAKE_DOCKER is ok, denied or down, FAKE_UP fails the first
# start, FAKE_RUNNING says the container runs.
DOCKER = r"""#!/bin/sh
echo "docker $*" >>"$FAKE_LOG"
if [ "$1" = info ]; then
  case ${FAKE_DOCKER:-ok} in
    denied) echo "permission denied while trying to connect to the Docker daemon socket at unix:///var/run/docker.sock" >&2; exit 1 ;;
    down) echo "Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?" >&2; exit 1 ;;
  esac
  exit 0
fi
case " $* " in
  *" up "*) [ -z "${FAKE_UP:-}" ] || { echo "address already in use" >&2; exit 1; } ;;
  *" ps "*) [ -z "${FAKE_RUNNING:-}" ] || echo 0123abcd ;;
  *" --control "*) echo '{"configured":false}' ;;
  *" --web "*) echo "Or open: http://127.0.0.1:$UAGENT_WEB_PORT/#pair=CODE" ;;
esac
exit 0
"""

# Serves the release's files from the checkout; FAKE_PORT=taken answers on the
# loopback port, else nothing listens there (curl's 7).
CURL = r"""#!/bin/sh
out= url=
while [ $# -gt 0 ]; do
  case $1 in -o) out=$2; shift ;; http*) url=$1 ;; esac
  shift
done
case $url in
  http://127.0.0.1:*) [ "${FAKE_PORT:-}" = taken ] && exit 0; exit 7 ;;
  */compose.yaml) cp "$FAKE_DEPLOY/compose.yaml" "$out" ;;
  */seccomp-chrome.json) cp "$FAKE_DEPLOY/seccomp-chrome.json" "$out" ;;
  *) exit 22 ;;
esac
"""


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory)
        self.home = self.directory / "home"
        self.home.mkdir()
        self.log = self.directory / "docker.log"
        self.log.touch()
        # The system's tools, but no Docker or curl but the fakes.
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        for folder in ("/usr/bin", "/bin", "/usr/sbin", "/sbin"):
            for tool in Path(folder).iterdir() if Path(folder).is_dir() else ():
                link = self.bin / tool.name
                if tool.name not in ("docker", "curl", "timedatectl") and not link.exists():
                    link.symlink_to(tool)
        self.fake("curl", CURL)
        self.script = self.directory / "release" / "install.sh"
        self.script.parent.mkdir()
        shutil.copy(ROOT / "install.sh", self.script)

    def fake(self, name, body):
        path = self.bin / name
        path.write_text(body)
        path.chmod(0o755)

    def install(self, docker="ok", **env):
        if docker:
            self.fake("docker", DOCKER)
        environment = {
            "PATH": str(self.bin),
            "HOME": str(self.home),
            "USER": "newuser",
            "UAGENT_VERSION": VERSION,
            "UAGENT_NONINTERACTIVE": "1",
            "UAGENT_WEB_PORT": PORT,
            "FAKE_LOG": str(self.log),
            "FAKE_DOCKER": docker or "",
            "FAKE_DEPLOY": str(ROOT / "deploy"),
            **env,
        }
        return subprocess.run(
            ["sh", str(self.script)],
            env=environment,
            cwd=self.home,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def calls(self):
        return self.log.read_text()

    def test_each_docker_state_is_named_with_its_fix(self):
        for state, said, fix in (
            (None, "Docker: missing", None),
            ("denied", "Docker is installed, but", "sudo usermod -aG docker"),
            ("down", "Docker is installed, but not running", "start"),
        ):
            with self.subTest(state=state):
                result = self.install(docker=state)
                self.assertIn(said, result.stdout)
                if fix:
                    self.assertIn("For the container: ", result.stdout)
                    self.assertIn(fix, result.stdout)

    def test_a_container_asked_for_stops_when_docker_cannot_run_it(self):
        for state, fix in (
            (None, None),
            ("denied", "sudo usermod -aG docker"),
            ("down", "start"),
        ):
            with self.subTest(state=state):
                result = self.install(docker=state, UAGENT_INSTALL="container")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("The container needs Docker, which is", result.stderr)
                if fix:
                    self.assertIn("First: " + fix, result.stderr)
                self.assertNotIn("compose", self.calls())


if __name__ == "__main__":
    unittest.main()
