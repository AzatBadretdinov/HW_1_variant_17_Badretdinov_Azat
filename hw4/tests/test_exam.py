"""Linux integration tests: real C clients plus controlled TCP peers."""
from collections import deque
import errno
import os
from pathlib import Path
import re
import signal
import socket
import subprocess as sp
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
RESULTS.mkdir(exist_ok=True)
PASSED = []


def read_log(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        if e.errno not in (errno.ENOENT, errno.ENODATA):
            raise
        return ""


class Session:
    def __init__(self, name, *args):
        self.name = name
        self.log = RESULTS / (name + ".log")
        self.log.unlink(missing_ok=True)
        self.stdout = tempfile.TemporaryFile()
        self.stderr = tempfile.TemporaryFile()
        self.children = []
        self.streams = []
        self.sockets = []
        self.server = sp.Popen([str(ROOT / "exam-server"), "--host", "127.0.0.1", "--port", "0",
            "--max-time", "1000", "--log", str(self.log), *map(str, args)],
            stdout=self.stdout, stderr=self.stderr)
        line = self.wait("LISTEN")
        self.host = "127.0.0.1"
        self.port = int(re.search(r"port=(\d+)", line)[1])

    def wait(self, marker, count=1):
        end = time.monotonic() + 6
        while time.monotonic() < end:
            data = read_log(self.log)
            if data.count(marker) >= count:
                return data
            assert self.server.poll() is None, (self.name, data)
            time.sleep(.005)
        raise AssertionError((self.name, marker, read_log(self.log)))

    def client(self, role, id=None):
        stream = tempfile.TemporaryFile()
        self.streams.append(stream)
        cmd = [str(ROOT / ("exam-" + role)), "--host", self.host, "--port", str(self.port)]
        if id is not None:
            cmd += ["--id", str(id)]
        p = sp.Popen(cmd, stdout=stream, stderr=self.stderr)
        self.children.append((p, stream, role))
        return p, stream

    def actors(self, students=6, teachers=2):
        for i in range(1, teachers + 1):
            self.client("teacher", i)
        for i in range(1, students + 1):
            self.client("student", i)

    def raw(self, hello=None):
        sock = socket.create_connection((self.host, self.port), timeout=3)
        sock.settimeout(3)
        self.sockets.append(sock)
        if hello is not None:
            sock.sendall(hello)
        return sock

    def finish(self, reason="complete", code=0, ignored=()):
        rc = self.server.wait(timeout=10)
        self.stderr.seek(0)
        assert rc == code, (self.name, rc, self.stderr.read().decode(), read_log(self.log))
        self.stdout.seek(0)
        text = self.stdout.read().decode()
        assert text == read_log(self.log)
        assert f"ИТОГ reason={reason} " in text, text
        for p, stream, role in self.children:
            status = p.wait(timeout=3)
            stream.seek(0)
            out = stream.read().decode()
            if p not in ignored:
                assert status == 0, (self.name, role, status, out)
                if role == "monitor":
                    assert "SNAPSHOT " in out and "END_SNAPSHOT " in out and "ИТОГ reason=" in out
                    events = list(map(int, re.findall(r"^EVENT (\d+)", out, re.M)))
                    if events:
                        assert events == list(range(events[0], events[-1] + 1)), out
        validate(text)
        PASSED.append(self.name)
        print("PASS", self.name)
        return text

    def close(self):
        for sock in self.sockets:
            sock.close()
        for p, _, _ in self.children:
            if p.poll() is None:
                p.terminate()
        if self.server.poll() is None:
            self.server.terminate()
        for p in [self.server, *(c[0] for c in self.children)]:
            try:
                p.wait(timeout=2)
            except sp.TimeoutExpired:
                p.kill()
                p.wait()
        for stream in [self.stdout, self.stderr, *self.streams]:
            stream.close()


def validate(text):
    fifo, busy, tickets, received = deque(), {}, {}, {}
    seq = list(map(int, re.findall(r"^EVENT (\d+)", text, re.M)))
    assert seq == list(range(1, len(seq) + 1))
    for line in text.splitlines():
        m = re.search(r"БИЛЕТ student=(\d+) ticket=(\d+)", line)
        if m:
            id, ticket = map(int, m.groups())
            assert id not in tickets and ticket not in tickets.values()
            tickets[id] = ticket
        m = re.search(r"ОЧЕРЕДЬ student=(\d+) size=(\d+) capacity=(\d+)", line)
        if m:
            id, size, capacity = map(int, m.groups())
            assert id not in fifo
            fifo.append(id)
            assert len(fifo) == size <= capacity
        m = re.search(r"ПРОВЕРКА teacher=(\d+) student=(\d+) job=(\d+)", line)
        if m:
            teacher, id, job = map(int, m.groups())
            assert teacher not in busy and fifo.popleft() == id
            busy[teacher] = (id, job)
        m = re.search(r"ВОЗВРАТ student=(\d+)", line)
        if m:
            id = int(m[1])
            matches = [t for t, v in busy.items() if v[0] == id]
            assert len(matches) == 1
            del busy[matches[0]]
        m = re.search(r"ПРОВЕРЕНО teacher=(\d+) student=(\d+) grade=(\d+) job=(\d+)", line)
        if m:
            teacher, id, grade, job = map(int, m.groups())
            assert busy.pop(teacher) == (id, job) and 2 <= grade <= 5
        m = re.search(r"ОЦЕНКА student=(\d+) grade=(\d+)", line)
        if m:
            id, grade = map(int, m.groups())
            assert id not in received and 2 <= grade <= 5
            received[id] = grade
    end = re.search(r"ИТОГ reason=(\w+) admitted=(\d+) completed=(\d+) unfinished=(\d+) rejected=(\d+)", text)
    assert end
    reason, admitted, completed, unfinished, rejected = end.groups()
    admitted, completed, unfinished, rejected = map(int, (admitted, completed, unfinished, rejected))
    assert admitted == completed + unfinished and completed == len(received)
    rows = re.findall(r"СТУДЕНТ id=(\d+) ticket=(\d+) state=(\w+) teacher=(\d+) grade=(\d+)", text)
    assert len(rows) == admitted + rejected
    assert sum(r[2] == "done" for r in rows) == completed
    for id, ticket, state, teacher, grade in rows:
        if state == "done":
            assert received[int(id)] == int(grade)
    if reason == "complete":
        assert unfinished == 0 and not busy and not fifo


def scenario(name, args, action, **finish):
    session = Session(name, *args)
    try:
        action(session)
        return session.finish(**finish)
    finally:
        session.close()


scenario("normal", [], lambda s: (s.client("monitor"), s.actors()))
text = scenario("queue_fixed", ["--queue", 1, "--prep-min", 1, "--prep-max", 1,
    "--check-time", 10, "--ticket-mode", "sequential", "--grade-mode", "ticket"],
    lambda s: (s.client("monitor"), s.actors(6, 1)))
assert "ОЖИДАНИЕ" in text
for id in range(1, 7):
    assert f"ОЦЕНКА student={id} grade={2 + (id - 1) % 4}" in text
text = scenario("shortage", ["--tickets", 2], lambda s: s.actors())
assert "admitted=2 completed=2 unfinished=0 rejected=4" in text
scenario("empty", ["--students", 0], lambda s: None)
scenario("no_tickets", ["--tickets", 0], lambda s: s.actors())
scenario("timeout", ["--max-time", 5, "--prep-min", 100, "--prep-max", 100],
         lambda s: (s.client("monitor"), s.actors()), reason="timeout")
scenario("missing_participant", ["--max-time", 30], lambda s: s.actors(5, 2), reason="timeout")
scenario("capacity", ["--students", 32, "--teachers", 32, "--tickets", 32,
         "--max-time", 3000], lambda s: (s.client("monitor"), s.actors(32, 32)))


def observers(s):
    monitors = [s.client("monitor")[0] for _ in range(3)]
    s.wait("role=WATCH", 3)
    monitors[0].terminate()
    assert monitors[0].wait(timeout=3) == 143
    s.wait("ОТКЛЮЧЕНИЕ role=watch")
    s.client("monitor")
    s.actors()
    return monitors[0]


s = Session("monitor_reconnect")
try:
    old = observers(s)
    s.finish(ignored=(old,))
finally:
    s.close()


for sig in (signal.SIGINT, signal.SIGTERM):
    def stop(s, sig=sig):
        s.client("monitor")
        s.actors()
        s.wait("ПРОВЕРКА ")
        s.server.send_signal(sig)
    scenario(f"server_signal_{int(sig)}", ["--check-time", 1000], stop,
             reason="interrupted", code=128 + int(sig))


def teacher_reconnect(s):
    old, _ = s.client("teacher", 1)
    s.client("student", 1)
    s.wait("ПРОВЕРКА ")
    old.kill()
    old.wait(timeout=3)
    s.wait("ВОЗВРАТ ")
    s.client("teacher", 1)
    return old


s = Session("teacher_reconnect", "--students", 1, "--teachers", 1, "--check-time", 50)
try:
    old = teacher_reconnect(s)
    s.finish(ignored=(old,))
finally:
    s.close()


s = Session("student_reconnect", "--students", 1, "--teachers", 1,
            "--prep-min", 30, "--prep-max", 30)
try:
    s.client("teacher", 1)
    old, _ = s.client("student", 1)
    s.wait("БИЛЕТ ")
    old.terminate()
    old.wait(timeout=3)
    s.wait("ОТКЛЮЧЕНИЕ role=student")
    s.client("student", 1)
    s.finish(ignored=(old,))
finally:
    s.close()


# A controlled student disconnects after submitting; the grade must survive offline.
s = Session("offline_result", "--students", 1, "--teachers", 1, "--check-time", 50)
try:
    s.client("teacher", 1)
    raw = s.raw(b"HELLO STUDENT 1\n")
    line = raw.makefile("rb").readline().decode()
    ticket = int(line.split()[1])
    raw.sendall(f"READY {ticket}\n".encode())
    s.wait("ПРОВЕРКА ")
    raw.shutdown(socket.SHUT_RDWR)
    raw.close()
    s.wait("ПРОВЕРЕНО ")
    s.client("student", 1)
    s.finish()
finally:
    s.close()


# Framing: a HELLO sent byte by byte must be accepted; malformed frames stay isolated.
s = Session("framing_and_rejection", "--students", 1, "--teachers", 1, "--check-time", 20)
try:
    watch = s.raw()
    for byte in b"HELLO WATCH 0\n":
        watch.sendall(bytes([byte]))
    s.wait("role=WATCH")
    teacher, _ = s.client("teacher", 1)
    s.wait("role=TEACHER")
    duplicate = s.raw(b"HELLO TEACHER 1\n")
    assert duplicate.recv(512).startswith(b"ERROR ")
    invalid = s.raw(b"nonsense\nHELLO STUDENT 1\n")
    assert invalid.recv(512).startswith(b"ERROR ")
    overlong = s.raw(b"x" * 700)
    assert overlong.recv(512) in (b"", b"ERROR invalid_message_or_duplicate_id\n")
    nul = s.raw(b"HELLO\x00WATCH 0\n")
    assert nul.recv(512) == b""
    s.client("student", 1)
    s.finish()
finally:
    s.close()


for seed in range(1, 6):
    scenario(f"stress_{seed}", ["--seed", seed, "--queue", 1, "--students", 20,
         "--tickets", 20, "--teachers", 4, "--max-time", 3000],
         lambda s: (s.client("monitor"), s.actors(20, 4)))

ip = sp.check_output(["hostname", "-I"], text=True).split()[0]
assert not ip.startswith("127."), ip
s = Session("network_interface", "--host", "0.0.0.0")
try:
    s.host = ip
    s.client("monitor")
    s.actors()
    s.finish()
finally:
    s.close()

for idx, args in enumerate([
    [], ["--host", "bad", "--port", 0], ["--host", "127.0.0.1", "--port", -1],
    ["--host", "127.0.0.1", "--port", 0, "--teachers", 0],
    ["--host", "127.0.0.1", "--port", 0, "--prep-min", 10, "--prep-max", 2],
    ["--host", "127.0.0.1", "--port", 0, "--seed", "abc"]]):
    p = sp.run([str(ROOT / "exam-server"), *map(str, args)], capture_output=True, timeout=3)
    assert p.returncode == 2
PASSED.append("invalid_options_6_cases")
for role in ("student", "teacher", "monitor"):
    p = sp.run([str(ROOT / ("exam-" + role)), "--host", "127.0.0.1", "--port", "1",
                *([] if role == "monitor" else ["--id", "1"])], capture_output=True, timeout=4)
    assert p.returncode == 2
PASSED.append("connection_refused_3_roles")
summary = f"{len(PASSED)} scenarios passed on Linux TCP (loopback and network interface).\n" + "\n".join(PASSED) + "\n"
(RESULTS / "test-summary.txt").write_text(summary, encoding="utf-8")
print(summary.splitlines()[0])
