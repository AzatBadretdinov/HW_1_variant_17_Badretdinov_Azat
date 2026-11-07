"""End-to-end checks of the same model over all four Linux IPC configurations."""
from collections import deque
import errno
import os
from pathlib import Path
import re
import signal
import subprocess as sp
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
RESULTS.mkdir(exist_ok=True)
MODES = ("unnamed", "named", "shm", "mq")
PASSED = []


def await_log(log, marker, owner):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            data = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        except OSError as error:
            # DrvFS may transiently return ENODATA while the writer appends.
            if error.errno != errno.ENODATA:
                raise
            data = ""
        if marker in data:
            return data
        assert owner.poll() is None, (log, owner.returncode, data)
        time.sleep(.005)
    raise AssertionError(f"Missing {marker!r}: {log}")


def validate(text, reason):
    queue, busy, started, graded, received = deque(), {}, set(), {}, set()
    tickets, pids = {}, set()
    concurrent = 0
    for line in text.splitlines():
        m = re.search(r"ПОДКЛЮЧЕНИЕ role=(\w+) id=(\d+) pid=(\d+)", line)
        if m:
            assert int(m[3]) not in pids
            pids.add(int(m[3]))
        m = re.search(r"Студент (\d+) получил и сообщил билет (\d+)", line)
        if m:
            id, ticket = map(int, m.groups())
            assert id not in tickets and ticket not in tickets.values()
            tickets[id] = ticket
        m = re.search(r"Студент (\d+) передал работу; поставлен в очередь \((\d+)/(\d+)\)", line)
        if m:
            id, length, capacity = map(int, m.groups())
            assert id in tickets and id not in queue and id not in started
            queue.append(id)
            assert len(queue) == length and length <= capacity
        m = re.search(r"Преподаватель (\d+) начал проверку работы студента (\d+)", line)
        if m:
            teacher, id = map(int, m.groups())
            assert teacher not in busy and id not in started and queue.popleft() == id
            started.add(id)
            busy[teacher] = id
            concurrent = max(concurrent, len(busy))
        m = re.search(r"Преподаватель (\d+) закончил проверку студента (\d+); оценка (\d+)", line)
        if m:
            teacher, id, grade = map(int, m.groups())
            assert busy.pop(teacher) == id and id not in graded and 2 <= grade <= 5
            graded[id] = (teacher, grade)
        m = re.search(r"Студент (\d+) получил свою оценку (\d+)", line)
        if m:
            id, grade = map(int, m.groups())
            assert id not in received and graded[id][1] == grade
            received.add(id)
    header = re.search(r"НАЧАЛО .*teachers=(\d+) students=(\d+).* pid=(\d+)", text)
    end = re.search(r"ИТОГ reason=(\w+) admitted=(\d+) completed=(\d+) unfinished=(\d+) rejected=(\d+)", text)
    assert header and end, text
    teachers, students, owner_pid = map(int, header.groups())
    assert owner_pid not in pids and end[1] == reason
    admitted, completed, unfinished, rejected = map(int, end.groups()[1:])
    assert admitted + rejected == students and completed + unfinished == admitted
    assert completed == len(received)
    rows = re.findall(r"СТУДЕНТ id=(\d+) ticket=(\d+) state=(\w+) teacher=(\d+) grade=(\d+)", text)
    assert len(rows) == students
    assert sum(row[2] == "not_admitted" for row in rows) == rejected
    assert sum(row[2] == "done" for row in rows) == completed
    for id, ticket, state, teacher, grade in rows:
        id, ticket, teacher, grade = map(int, (id, ticket, teacher, grade))
        if state in ("done", "graded"):
            assert graded[id] == (teacher, grade)
        elif state == "not_admitted":
            assert ticket == grade == 0
        else:
            assert grade == 0
    if reason == "complete":
        assert not queue and not busy and not unfinished
        assert len(pids) == students + teachers and len(tickets) == admitted
    return concurrent


def ipc_objects(mode, session):
    prefix = f"prj17_{os.getuid()}_{MODES.index(mode)}_{session}_"
    return [str(entry) for directory in (Path("/dev/shm"), Path("/dev/mqueue"))
            if directory.is_dir() for entry in directory.iterdir()
            if entry.name.removeprefix("sem.").startswith(prefix)]


def exercise(mode, name, args=(), students=6, teachers=2, reason="complete", sig=None,
             omit_student=False, collision=False, victim=False):
    label = f"{mode}_{name}"
    log = RESULTS / f"{label}.log"
    log.unlink(missing_ok=True)
    external = mode in ("shm", "mq")
    session = "test_" + uuid.uuid4().hex[:12]
    cmd = [str(ROOT / f"exam-{mode}"), "--students", str(students), "--teachers",
           str(teachers), "--log", str(log), *map(str, args)]
    if external:
        cmd += ["--session", session]
    children = []
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as err:
        owner = sp.Popen(cmd, stdout=output, stderr=err)
        def actor(role, number):
            return [str(ROOT / f"exam-{role}"), "--mode", mode, "--session",
                    session, "--id", str(number)]
        try:
            if external:
                await_log(log, "ГОТОВО", owner)
                if collision:
                    other_log = RESULTS / "unexpected-collision.log"
                    another = sp.run(cmd + ["--log", str(other_log)], capture_output=True, timeout=3)
                    assert another.returncode == 2 and not other_log.exists()
                for role, count in (("teacher", teachers), ("student", students - omit_student)):
                    for number in range(1, count + 1):
                        children.append(sp.Popen(actor(role, number), stdout=sp.DEVNULL, stderr=err))
                if collision:
                    await_log(log, "role=teacher id=1", owner)
                    duplicate = sp.run(actor("teacher", 1), capture_output=True, timeout=3)
                    assert duplicate.returncode == 2
            if sig or victim:
                data = await_log(log, "ожидает места", owner)
                session = re.search(r"НАЧАЛО .*session=([\w-]+)", data)[1]
                names = ipc_objects(mode, session)
                assert any("/sem." in obj for obj in names) == (mode != "unnamed"), names
                assert any("/dev/mqueue/" in obj for obj in names) == (mode == "mq"), names
                assert any("/dev/shm/prj17_" in obj for obj in names) == (mode != "mq"), names
                if victim:
                    pid = int(re.search(r"role=teacher id=1 pid=(\d+)", data)[1])
                    os.kill(pid, signal.SIGTERM if external else signal.SIGKILL)
                else:
                    owner.send_signal(sig)
            rc = owner.wait(timeout=10)
            expected = 128 + sig if sig else 2 if victim else 0
            err.seek(0)
            assert rc == expected, (label, rc, err.read().decode())
            for p in children:
                status = p.wait(timeout=3)
                assert status == (143 if victim and p is children[0] else 0), (label, status)
            output.seek(0)
            data = output.read().decode()
            assert data == log.read_text(encoding="utf-8")
            session = re.search(r"НАЧАЛО .*session=([\w-]+)", data)[1]
            concurrency = validate(data, reason)
            assert not ipc_objects(mode, session), ipc_objects(mode, session)
            if not external:
                for pid in re.findall(r"ПОДКЛЮЧЕНИЕ .* pid=(\d+)", data):
                    assert not Path(f"/proc/{pid}").exists(), pid
            PASSED.append(label)
            print("PASS", label)
            return data, concurrency
        finally:
            for p in [owner, *children]:
                if p.poll() is None:
                    p.terminate()
            for p in [owner, *children]:
                try:
                    p.wait(timeout=2)
                except sp.TimeoutExpired:
                    p.kill()
                    p.wait()


blocked = ["--tickets", 12, "--queue", 1, "--prep-min", 1, "--prep-max", 1,
           "--check-time", 400, "--max-time", 0]
for mode in MODES:
    data, concurrent = exercise(mode, "normal", ["--max-time", 0, "--check-time", 10])
    assert "admitted=6 completed=6 unfinished=0 rejected=0" in data and concurrent >= 2
    data, _ = exercise(mode, "queue", ["--tickets", 12, "--queue", 1,
        "--prep-min", 1, "--prep-max", 1, "--check-time", 8, "--max-time", 0,
        "--ticket-mode", "sequential", "--grade-mode", "ticket"], students=12, teachers=1)
    assert "peak_queue=1" in data and "ожидает места" in data
    for id in range(1, 13):
        assert f"Студент {id} получил свою оценку {2 + (id - 1) % 4}" in data
    data, _ = exercise(mode, "shortage", ["--tickets", 2, "--max-time", 0])
    assert "admitted=2 completed=2 unfinished=0 rejected=4" in data
    exercise(mode, "empty", students=0)
    exercise(mode, "no_tickets", ["--tickets", 0])
    data, _ = exercise(mode, "capacity", ["--tickets", 32, "--max-time", 0], students=32, teachers=32)
    assert "admitted=32 completed=32 unfinished=0" in data
    exercise(mode, "timeout", ["--check-time", 400, "--max-time", 30], reason="timeout")
    for sig in (signal.SIGINT, signal.SIGTERM):
        exercise(mode, f"signal_{int(sig)}", blocked, students=12, teachers=1,
                 sig=sig, reason="interrupted")
    exercise(mode, "participant_lost", blocked, students=12, teachers=1,
             victim=True, reason="participant_error")
    for extra in (["--teachers", 0], ["--prep-min", 10, "--prep-max", 1], ["--queue", "bad"]):
        p = sp.run([str(ROOT / f"exam-{mode}"), *map(str, extra)], capture_output=True, timeout=3)
        assert p.returncode == 2
    PASSED.append(f"{mode}_invalid_options")
    if mode in ("shm", "mq"):
        exercise(mode, "missing_student", ["--max-time", 30], omit_student=True, reason="timeout")
        exercise(mode, "duplicate_session_and_actor", ["--check-time", 30, "--max-time", 0],
                 collision=True)
summary = f"{len(PASSED)} test scenarios passed on Linux.\n" + "\n".join(PASSED) + "\n"
(RESULTS / "test-summary.txt").write_text(summary, encoding="utf-8")
print(summary.splitlines()[0])
