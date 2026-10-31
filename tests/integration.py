import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


class Session:
    def __init__(self, plugin, artifacts):
        display = Path(os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
        if not display.is_absolute():
            display = Path(os.environ["XDG_RUNTIME_DIR"]) / display
        if not display.is_socket():
            raise RuntimeError(f"integration tests need a running Wayland session: {display}")
        self.plugin = plugin.resolve()
        self.artifacts = artifacts.resolve()
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.runtime = Path(tempfile.mkdtemp(prefix="wss-"))
        self.process = None
        self.clients = []
        self.logs = []
        self.instance = None
        self.env = os.environ.copy()
        self.env.update({
            "WAYLAND_DISPLAY": str(display),
            "XDG_RUNTIME_DIR": str(self.runtime),
            "XDG_CACHE_HOME": str(self.runtime / "cache"),
            "XDG_CONFIG_HOME": str(self.runtime / "config"),
            "XDG_DATA_HOME": str(self.runtime / "data"),
            "XDG_STATE_HOME": str(self.runtime / "state"),
            "LIBSEAT_BACKEND": "workspace-swap-test-no-seat",
            "HYPRLAND_NO_SD_VARS": "1",
            "HYPRLAND_NO_SD_NOTIFY": "1",
            "HYPRLAND_NO_SD_TARGET": "1",
            "HYPRLAND_NO_CRASHREPORTER": "1",
        })
        for name in ("HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_SOCKET", "DISPLAY", "SWAYSOCK", "NOTIFY_SOCKET", "DBUS_SESSION_BUS_ADDRESS"):
            self.env.pop(name, None)

    def __enter__(self):
        try:
            log = (self.artifacts / "hyprland.log").open("w")
            self.logs.append(log)
            self.process = subprocess.Popen(
                ["Hyprland", "--config", str(Path(__file__).with_name("session.lua"))],
                env=self.env, stdout=log, stderr=subprocess.STDOUT,
            )
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(f"nested Hyprland exited: {self.process.returncode}; see {self.artifacts}")
                for lock in self.runtime.glob("hypr/*/hyprland.lock"):
                    pid, display = lock.read_text().splitlines()[:2]
                    if int(pid) == self.process.pid and (lock.parent / ".socket.sock").exists():
                        self.instance = lock.parent
                        self.env["WAYLAND_DISPLAY"] = display
                        self.env["HYPRLAND_INSTANCE_SIGNATURE"] = lock.parent.name
                        self.request("/version")
                        errors = self.request("/configerrors").strip()
                        assert not errors, errors
                        assert self.request("/output create headless WSS-1").strip() == "ok"
                        monitors = json.loads(self.request("j/monitors"))
                        assert len(monitors) == 1 and monitors[0]["name"] == "WSS-1", monitors
                        outputs = json.loads(self.request("j/monitors all"))
                        nested = [output for output in outputs if output["name"].startswith("WAYLAND-")]
                        assert nested and all(output["disabled"] for output in nested), outputs
                        print("PASS: native headless test output, nested output disabled", flush=True)
                        result = self.request(f"/plugin load {self.plugin}").strip()
                        assert result == "ok", result
                        return self
                time.sleep(0.1)
            raise TimeoutError(f"nested Hyprland did not start; see {self.artifacts}")
        except BaseException:
            self.close()
            raise

    def request(self, command):
        assert self.instance is not None
        assert self.instance.is_relative_to(self.runtime)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(8)
            connection.connect(str(self.instance / ".socket.sock"))
            connection.sendall(command.encode())
            chunks = []
            while chunk := connection.recv(65536):
                chunks.append(chunk)
        return b"".join(chunks).decode()

    def evaluate(self, code):
        result = self.request("r/eval " + code).strip()
        assert result == "ok", (code, result)

    def dispatch(self, expression):
        self.evaluate("assert(hl.dispatch(" + expression + "))")

    def focus(self, workspace):
        self.dispatch("hl.dsp.focus({ workspace = " + json.dumps(str(workspace)) + " })")

    def snapshot(self):
        clients = json.loads(self.request("j/clients"))
        return {client["class"]: client for client in clients if client["class"].startswith("wss-")}

    def spawn(self, name):
        name = "wss-" + name
        log = (self.artifacts / f"{name}.log").open("w")
        self.logs.append(log)
        process = subprocess.Popen(
            ["kitty", "--config", "NONE", "--class", name, "--title", name, "/bin/sleep", "300"],
            env=self.env, stdout=log, stderr=subprocess.STDOUT,
        )
        self.clients.append(process)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"kitty {name} exited; see {self.artifacts}")
            if name in self.snapshot():
                time.sleep(0.15)
                return name
            time.sleep(0.05)
        raise TimeoutError(f"kitty {name} did not map")

    def selector(self, name):
        return json.dumps("address:" + self.snapshot()[name]["address"])

    def window_command(self, command, name, fields=""):
        self.dispatch(f"hl.dsp.window.{command}({{ window = {self.selector(name)}{fields} }})")

    def toggle(self, bank):
        self.evaluate(f"assert(hl.plugin.workspace_swap.toggle()); assert(hl.plugin.workspace_swap.bank() == {bank})")
        time.sleep(0.1)

    def close(self):
        for process in self.clients:
            if process.poll() is None:
                process.terminate()
        for process in self.clients:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for log in self.logs:
            log.close()
        if self.instance and (self.instance / "hyprland.log").exists():
            shutil.copyfile(self.instance / "hyprland.log", self.artifacts / "hyprland-runtime.log")
        shutil.rmtree(self.runtime)

    def __exit__(self, *args):
        self.close()


def stable_state(snapshot):
    return {
        name: {key: client[key] for key in ("address", "workspace", "at", "size", "floating", "monitor", "fullscreen", "fullscreenClient", "fullscreenHandler", "grouped", "hidden", "pinned", "swallowing", "acceptsInput")}
        for name, client in snapshot.items()
    }


def basic_round_trip(session):
    session.toggle(2)
    session.toggle(1)
    workspaces = json.loads(session.request("j/workspaces"))
    assert sorted(w["id"] for w in workspaces if "id" in w) == list(range(1, 11)), workspaces
    session.focus(1)
    for name in ("a1", "a2", "a3"):
        session.spawn(name)
    session.dispatch('hl.dsp.layout("mfact exact 0.67")')
    session.focus(2)
    session.spawn("other")
    session.focus(1)
    session.dispatch('hl.dsp.workspace.toggle_special("1")')
    for name in ("b1", "b2", "b3"):
        session.spawn(name)
    session.dispatch('hl.dsp.layout("mfact exact 0.42")')
    session.dispatch('hl.dsp.layout("orientationright")')
    session.dispatch('hl.dsp.workspace.toggle_special("1")')
    time.sleep(0.15)
    before = stable_state(session.snapshot())
    session.toggle(2)
    after = session.snapshot()
    for name, client in before.items():
        old = client["workspace"]["address"]
        expected = old.removeprefix("special:") if old.startswith("special:") else "special:" + old
        assert after[name]["workspace"]["address"] == expected, (name, after[name])
        assert after[name]["at"] == client["at"], (name, after[name]["at"], client["at"])
        assert after[name]["size"] == client["size"], (name, after[name]["size"], client["size"])
    session.toggle(1)
    assert stable_state(session.snapshot()) == before, (before, stable_state(session.snapshot()))
    session.focus(1)
    session.dispatch('hl.dsp.focus({ workspace = "e-1" })')
    assert json.loads(session.request("j/activeworkspace"))["id"] == 10
    print("PASS: empty banks, ten ordinary IDs, all-pair content and master-layout round trip, native cycling", flush=True)


def floating_groups_and_fullscreen(session):
    session.focus(3)
    floating = session.spawn("float-a")
    session.window_command("float", floating, ', action = "set"')
    session.window_command("resize", floating, ", x = 480, y = 280")
    session.window_command("move", floating, ", x = 143, y = 91")
    session.dispatch('hl.dsp.workspace.toggle_special("3")')
    floating_b = session.spawn("float-b")
    session.window_command("float", floating_b, ', action = "set"')
    session.window_command("resize", floating_b, ", x = 410, y = 230")
    session.window_command("move", floating_b, ", x = 211, y = 137")
    session.dispatch('hl.dsp.workspace.toggle_special("3")')

    session.focus(4)
    session.spawn("group-a1")
    session.dispatch("hl.dsp.group.toggle()")
    session.spawn("group-a2")
    assert len(session.snapshot()["wss-group-a1"]["grouped"]) == 2
    session.dispatch('hl.dsp.workspace.toggle_special("4")')
    session.spawn("group-b1")
    session.dispatch("hl.dsp.group.toggle()")
    session.spawn("group-b2")
    assert len(session.snapshot()["wss-group-b1"]["grouped"]) == 2
    session.dispatch('hl.dsp.workspace.toggle_special("4")')

    session.focus(5)
    fullscreen = session.spawn("fs-a")
    session.window_command("fullscreen", fullscreen, ', action = "set", mode = "fullscreen"')
    session.dispatch('hl.dsp.workspace.toggle_special("5")')
    maximized = session.spawn("fs-b")
    session.window_command("fullscreen", maximized, ', action = "set", mode = "maximized"')
    session.dispatch('hl.dsp.workspace.toggle_special("5")')
    before = stable_state(session.snapshot())
    assert before[fullscreen]["fullscreen"] == 2
    assert before[maximized]["fullscreen"] == 1
    session.toggle(2)
    assert json.loads(session.request("j/activewindow"))["class"] == maximized
    after = session.snapshot()
    for name, client in before.items():
        assert after[name]["at"] == client["at"], (name, after[name]["at"], client["at"])
        assert after[name]["size"] == client["size"], (name, after[name]["size"], client["size"])
        for key in ("fullscreen", "fullscreenClient", "fullscreenHandler", "grouped", "hidden", "floating"):
            assert after[name][key] == client[key], (name, key, after[name][key], client[key])
    session.toggle(1)
    assert json.loads(session.request("j/activewindow"))["class"] == fullscreen
    assert stable_state(session.snapshot()) == before
    session.focus(4)
    session.dispatch("hl.dsp.group.next()")
    assert not session.snapshot()["wss-group-a1"]["hidden"]
    session.toggle(2)
    session.dispatch("hl.dsp.group.next()")
    assert not session.snapshot()["wss-group-b1"]["hidden"]
    session.toggle(1)
    print("PASS: floating geometry, hidden group members and group cycling, fullscreen/maximized modes and focus", flush=True)


def rejection_checks(session):
    session.focus(3)
    session.window_command("pin", "wss-float-a", ', action = "set"')
    before = stable_state(session.snapshot())
    session.evaluate('local ok, err = hl.plugin.workspace_swap.toggle(); assert(not ok and err:find("unpin")); assert(hl.plugin.workspace_swap.bank() == 1)')
    assert stable_state(session.snapshot()) == before
    session.window_command("pin", "wss-float-a", ', action = "unset"')
    session.dispatch('hl.dsp.workspace.toggle_special("3")')
    before = stable_state(session.snapshot())
    session.evaluate('local ok, err = hl.plugin.workspace_swap.toggle(); assert(not ok and err:find("close")); assert(hl.plugin.workspace_swap.bank() == 1)')
    assert stable_state(session.snapshot()) == before
    session.dispatch('hl.dsp.workspace.toggle_special("3")')
    print("PASS: pinned-window and visible-private-workspace rejection without partial swaps", flush=True)


def notification_checks(session):
    expected = {}
    for client in session.snapshot().values():
        old = client["workspace"]["address"]
        expected[client["address"]] = old.removeprefix("special:") if old.startswith("special:") else "special:" + old
    entries = ",".join("[" + json.dumps(address) + "]=" + json.dumps(destination) for address, destination in expected.items())
    session.evaluate("""
        _G.wss_expected = {""" + entries + """}; _G.wss_callbacks = 0; _G.wss_callback_error = nil
        _G.wss_subscription = hl.on("window.move_to_workspace", function()
            if _G.wss_expected == nil then return end
            local ok, err = pcall(function()
                assert(hl.plugin.workspace_swap.bank() == 2)
                for _, window in ipairs(hl.get_windows()) do
                    local destination = _G.wss_expected[window.address]
                    if destination then assert(window.workspace.addressable_name == destination) end
                end
                local nested, message = hl.plugin.workspace_swap.toggle()
                assert(not nested and message:find("already"))
            end)
            if not ok then _G.wss_callback_error = err end
            _G.wss_callbacks = _G.wss_callbacks + 1
        end)
    """)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as events:
        events.settimeout(2)
        events.connect(str(session.instance / ".socket2.sock"))
        session.toggle(2)
        received = b""
        while b"workspace_swap>>2\n" not in received:
            received += events.recv(65536)
        text = received.decode()
        for address, destination in expected.items():
            assert f"movewindowv2>>{address.removeprefix('0x')},{destination}," in text, text
    session.evaluate(f'assert(_G.wss_callback_error == nil, _G.wss_callback_error); assert(_G.wss_callbacks == {len(expected)}); _G.wss_expected = nil')
    session.toggle(1)
    print("PASS: consistent batch state in synchronous callbacks, reentrant rejection, window and bank IPC events", flush=True)


def lifecycle_checks(session):
    session.focus(1)
    session.toggle(2)
    newcomer = session.spawn("new-bank-b")
    session.toggle(1)
    assert session.snapshot()[newcomer]["workspace"]["address"] == "special:1"
    process = session.clients[-1]
    process.terminate()
    process.wait(timeout=3)
    deadline = time.monotonic() + 5
    while newcomer in session.snapshot() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert newcomer not in session.snapshot()
    session.toggle(2)
    before = stable_state(session.snapshot())
    assert session.request("/reload config-only").strip() == "ok"
    session.evaluate("assert(hl.plugin.workspace_swap.bank() == 2)")
    assert stable_state(session.snapshot()) == before
    session.toggle(1)
    print("PASS: new/closed windows in the correct bank, config reload preserves content and layout", flush=True)


def multiple_monitors(session):
    session.evaluate('hl.workspace_rule({ workspace = "10", monitor = "WSS-2", default = true })')
    assert session.request("/output create headless WSS-2").strip() == "ok"
    session.dispatch('hl.dsp.workspace.move({ workspace = "3", monitor = "WSS-2" })')
    session.focus(3)
    before = session.snapshot()
    monitors = json.loads(session.request("j/monitors"))
    positions = {monitor["id"]: [monitor["x"], monitor["y"]] for monitor in monitors}
    incoming = before["wss-float-b"]
    relative = [incoming["at"][i] - positions[incoming["monitor"]][i] for i in range(2)]
    target_monitor = before["wss-float-a"]["monitor"]
    assert incoming["monitor"] != target_monitor
    session.toggle(2)
    after = session.snapshot()["wss-float-b"]
    assert after["monitor"] == target_monitor
    assert [after["at"][i] - positions[target_monitor][i] for i in range(2)] == relative
    assert after["size"] == incoming["size"]
    session.toggle(1)
    assert session.snapshot()["wss-float-a"]["at"] == before["wss-float-a"]["at"]
    assert session.snapshot()["wss-float-a"]["size"] == before["wss-float-a"]["size"]
    print("PASS: global multi-monitor swap and monitor-relative floating placement", flush=True)


def dwindle_round_trip(session):
    session.evaluate('hl.config({ general = { layout = "dwindle" } })')
    session.focus(1)
    session.dispatch('hl.dsp.layout("togglesplit")')
    session.dispatch('hl.dsp.layout("splitratio 0.63 exact")')
    before = stable_state(session.snapshot())
    session.toggle(2)
    session.toggle(1)
    assert stable_state(session.snapshot()) == before
    print("PASS: dwindle split-tree round trip", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Run isolated Hyprland tests on headless outputs using the current Wayland session.")
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    arguments = parser.parse_args()
    with Session(arguments.plugin, arguments.artifacts) as session:
        basic_round_trip(session)
        floating_groups_and_fullscreen(session)
        rejection_checks(session)
        notification_checks(session)
        lifecycle_checks(session)
        multiple_monitors(session)
        dwindle_round_trip(session)
        assert session.request("/reload config-only").strip() == "ok"
        session.toggle(2)
        before = stable_state(session.snapshot())
        assert session.request(f"/plugin unload {session.plugin}").strip() == "ok"
        assert stable_state(session.snapshot()) == before
        session.evaluate("assert(hl.plugin.workspace_swap == nil or hl.plugin.workspace_swap.toggle == nil)")
        session.dispatch('hl.dsp.workspace.toggle_special("1")')
        assert session.snapshot()["wss-a1"]["workspace"]["address"] == "special:1"
        print("PASS: clean plugin unload keeps native workspaces and recovery access", flush=True)


if __name__ == "__main__":
    main()
