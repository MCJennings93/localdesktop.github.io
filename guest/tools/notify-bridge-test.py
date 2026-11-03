#!/usr/bin/env python3
"""Tests the notification bridge's guest agent, on any PC with python-gobject and dbus-run-session:

    dbus-run-session -- python3 guest/tools/notify-bridge-test.py

It owns org.freedesktop.Notifications with a fake desktop server, starts the real agent, and plays
the app's part on its socket. A second process sends Linux notifications, as an app would.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

HERE = os.path.dirname(os.path.abspath(__file__))
AGENT = os.path.join(HERE, "../managed/usr/local/bin/localdesktop-notify-bridge")
XML = """<node><interface name='org.freedesktop.Notifications'>
<method name='Notify'><arg type='s'/><arg type='u'/><arg type='s'/><arg type='s'/><arg type='s'/>
<arg type='as'/><arg type='a{sv}'/><arg type='i'/><arg type='u' direction='out'/></method>
<method name='CloseNotification'><arg type='u'/></method>
<signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>
<signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>
</interface></node>"""
PATH, NAME = "/org/freedesktop/Notifications", "org.freedesktop.Notifications"


class Server:
    """What a desktop's notification server does, with the calls it got on record."""

    def __init__(self):
        self.next_id, self.shown, self.connection = 1, {}, None
        self.ready = threading.Event()
        info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]

        def acquired(connection, _name):
            self.connection = connection
            connection.register_object(PATH, info, self.method, None, None)

        Gio.bus_own_name(Gio.BusType.SESSION, NAME, 0, acquired, lambda *_: self.ready.set(), None)

    def method(self, connection, _sender, _path, _interface, method, parameters, invocation):
        if method == "Notify":
            app, replaces, _icon, title, body, actions, hints, timeout = parameters.unpack()
            notification_id = replaces or self.next_id
            self.next_id = max(self.next_id, notification_id + 1)
            self.shown[notification_id] = dict(app=app, title=title, body=body, actions=actions, hints=hints, timeout=timeout)
            invocation.return_value(GLib.Variant("(u)", (notification_id,)))
        else:
            notification_id = parameters.unpack()[0]
            self.shown.pop(notification_id, None)
            invocation.return_value(None)
            self.emit("NotificationClosed", GLib.Variant("(uu)", (notification_id, 3)))

    def emit(self, signal, parameters):
        self.connection.emit_signal(None, PATH, NAME, signal, parameters)

    def user_dismisses(self, notification_id):
        self.shown.pop(notification_id, None)
        self.emit("NotificationClosed", GLib.Variant("(uu)", (notification_id, 2)))

    def expires(self, notification_id):
        self.emit("NotificationClosed", GLib.Variant("(uu)", (notification_id, 1)))


failures = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if not ok else ""), flush=True)
    if not ok:
        failures.append(name)


class App:
    """The app's end of the socket."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.05)
        self.sock.settimeout(0.05)
        self.buffer = b""

    def send(self, **message):
        self.sock.sendall(json.dumps(message).encode() + b"\n")

    def raw(self, data):
        self.sock.sendall(data)

    def recv(self, wait=1.5):
        """The next message, or None if nothing came in time."""
        end = time.time() + wait
        while time.time() < end:
            if b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                return json.loads(line)
            try:
                self.buffer += self.sock.recv(65536)
            except socket.timeout:
                pass
        return None


def notify(title, body="", replaces=0, app="testapp"):
    out = subprocess.run(
        ["gdbus", "call", "--session", "--dest", NAME, "--object-path", PATH, "--method", NAME + ".Notify",
         app, str(replaces), "", title, body, "[]", "{}", "5000"],
        capture_output=True, text=True, check=True).stdout
    return int(out.split()[1].rstrip(",)"))


def wait_for(condition, wait=1.5):
    end = time.time() + wait
    while time.time() < end:
        if condition():
            return True
        time.sleep(0.02)
    return False


def tests(server, path):
    app = App(path)
    # Linux to Android.
    nid = notify("Hello", "a <b>bold</b> &amp; plain body")
    m = app.recv()
    check("a Linux notification reaches Android, with its id and plain text",
          m == dict(type="post", id=nid, app="testapp", title="Hello", body="a bold & plain body"), m)
    notify("Hello again", replaces=nid)
    m = app.recv()
    check("an update keeps the id", m and m["id"] == nid and m["title"] == "Hello again", m)
    server.expires(nid)
    check("a popup timing out leaves the Android copy", app.recv(0.4) is None)
    server.user_dismisses(nid)
    check("dismissed on Linux: closed on Android", app.recv() == dict(type="close", id=nid))
    nid = notify("Second")
    app.recv()
    app.send(type="dismissed", id=nid)
    check("dismissed on Android: closed on Linux", wait_for(lambda: nid not in server.shown))
    check("that close isn't echoed back", app.recv(0.4) is None)

    # Android to Linux.
    app.send(type="post", key="k1", app="Mail", title="A <new> message", body="x & y")
    check("an Android notification shows on Linux", wait_for(lambda: any(n["app"] == "Mail" for n in server.shown.values())))
    aid, shown = next((i, n) for i, n in server.shown.items() if n["app"] == "Mail")
    check("escaped, never expiring, clickable, marked as ours",
          shown["title"] == "A &lt;new&gt; message" and shown["body"] == "x &amp; y" and shown["timeout"] == 0
          and "default" in shown["actions"] and "x-localdesktop-origin" in shown["hints"], shown)
    check("it isn't echoed back to Android", app.recv(0.4) is None)
    app.send(type="post", key="k1", app="Mail", title="Updated", body="")
    check("an update replaces it", wait_for(lambda: server.shown.get(aid, {}).get("title") == "Updated"))
    server.emit("ActionInvoked", GLib.Variant("(us)", (aid, "default")))
    check("a click opens it on Android", app.recv() == dict(type="open", key="k1"))
    server.user_dismisses(aid)
    check("dismissed on Linux: cancelled on Android", app.recv() == dict(type="cancel", key="k1"))
    app.send(type="post", key="k2", app="Chat", title="Hi", body="")
    assert wait_for(lambda: any(n["app"] == "Chat" for n in server.shown.values()))
    aid = next(i for i, n in server.shown.items() if n["app"] == "Chat")
    app.send(type="remove", key="k2")
    check("removed on Android: closed on Linux", wait_for(lambda: aid not in server.shown))
    check("no cancel comes back for that", app.recv(0.4) is None)

    # Untrusted input.
    app.raw(b"not json\n[1,2]\n{\"type\": \"post\"}\n{\"type\": \"post\", \"key\": 1, \"app\": 2, \"title\": 3, \"body\": 4}\n"
            b"{\"type\": \"dismissed\", \"id\": \"x\"}\n{\"type\": \"dismissed\", \"id\": 99999}\n{\"type\": \"remove\"}\n"
            b"{\"type\": \"run\", \"command\": \"touch /tmp/pwned\"}\n" + b"y" * 70000 + b"\n" + b"\xff\xfe\n")
    app.send(type="post", key="k3", app="After", title="still alive", body="")
    check("bad input is ignored and the agent keeps going",
          wait_for(lambda: any(n["app"] == "After" for n in server.shown.values())))
    aid = next(i for i, n in server.shown.items() if n["app"] == "After")

    # The app going away and coming back.
    app.sock.close()
    check("when the app goes, its notifications leave the desktop", wait_for(lambda: aid not in server.shown))
    nid = notify("While away")
    time.sleep(0.2)
    app = App(path)
    m = app.recv()
    check("what arrived meanwhile is delivered on reconnect", m and m["id"] == nid and m["title"] == "While away", m)
    app.send(type="dismissed", id=nid)
    check("and can still be dismissed", wait_for(lambda: nid not in server.shown))


def main():
    server = Server()
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()
    wait_for(lambda: server.connection is not None)
    time.sleep(0.2)
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "notify.sock")
        agent = subprocess.Popen([sys.executable, AGENT], env={**os.environ, "LOCALDESKTOP_NOTIFY_SOCKET": path})
        try:
            tests(server, path)
            check("the agent is still running", agent.poll() is None)
        finally:
            agent.terminate()
    print("RESULT:", f"FAIL ({len(failures)})" if failures else "PASS")
    sys.exit(bool(failures))


main()
