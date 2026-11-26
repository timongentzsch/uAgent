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

    def test_a_taken_port_stops_before_anything_starts(self):
        result = self.install(UAGENT_INSTALL="container", FAKE_PORT="taken")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"Port {PORT} on 127.0.0.1 is in use", result.stderr)
        self.assertIn("UAGENT_WEB_PORT", result.stderr)
        self.assertNotIn(" up ", self.calls())

    def test_the_port_is_ours_when_the_container_already_runs(self):
        result = self.install(UAGENT_INSTALL="container", FAKE_PORT="taken", FAKE_RUNNING="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(" up ", self.calls())

    def test_a_failed_start_is_stopped_and_says_what_to_do(self):
        result = self.install(UAGENT_INSTALL="container", FAKE_UP="1")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("µAgent's container did not start", result.stderr)
        self.assertIn(" stop", self.calls())

    def test_a_container_install_keeps_its_port(self):
        result = self.install(UAGENT_INSTALL="container", UAGENT_WEB_PORT="8181")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        env = (self.home / ".local/share/uagent/container/deploy/.env").read_text()
        self.assertIn("UAGENT_WEB_PORT=8181\n", env)

    def run_command(self, *args, **env):
        environment = {"PATH": str(self.bin), "FAKE_LOG": str(self.log), **env}
        return subprocess.run(
            [str(self.home / ".local/bin/uagent"), *args],
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_a_container_install_answers_to_uagent(self):
        result = self.install(UAGENT_INSTALL="container")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("to pair another device later: uagent --web", result.stdout)
        # Not running: says how to start it; running: the container's, without
        # a terminal here.
        stopped = self.run_command("--version")
        self.assertEqual(stopped.returncode, 1)
        self.assertIn("µAgent's container is not running", stopped.stderr)
        self.assertEqual(self.run_command("setup", FAKE_RUNNING="1").returncode, 0)
        self.assertIn("exec -T uagent uagent setup", self.calls())

    def test_a_container_install_leaves_a_native_uagent_alone(self):
        native = self.home / ".local/bin/uagent"
        native.parent.mkdir(parents=True)
        native.write_text("#!/bin/sh\necho native\n")
        (self.home / ".local/share/uagent").mkdir(parents=True)
        (self.home / ".local/share/uagent/install").write_text("release\n")
        result = self.install(UAGENT_INSTALL="container")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(native.read_text(), "#!/bin/sh\necho native\n")
        self.assertIn("the container's is: docker compose -f", result.stdout)

    def test_the_link_says_how_another_device_reaches_it(self):
        local = self.install(UAGENT_INSTALL="container")
        self.assertIn("A pairing link works once, within 5 minutes", local.stdout)
        self.assertNotIn("ssh -L", local.stdout)
        remote = self.install(
            UAGENT_INSTALL="container", SSH_CONNECTION="203.0.113.7 50000 198.51.100.2 22"
        )
        self.assertIn(f"ssh -L {PORT}:127.0.0.1:{PORT} ", remote.stdout)
        self.assertIn("@198.51.100.2, then open the link there", remote.stdout)
        self.assertIn("docs/operations/remote-access.md", remote.stdout)


if __name__ == "__main__":
    unittest.main()
