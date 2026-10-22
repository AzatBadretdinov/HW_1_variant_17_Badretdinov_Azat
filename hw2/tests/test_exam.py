"""Integration checks for concurrent invariants, not a fixed thread schedule."""
from collections import deque
from pathlib import Path
import os
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / ('exam.exe' if os.name == 'nt' else 'exam')
RESULTS = ROOT / 'results'
RESULTS.mkdir(exist_ok=True)
passed = []


def validate(text):
    queue, busy, started, finished, received = deque(), {}, set(), {}, {}
    tickets = {}
    maximum_busy = 0
    for line in text.splitlines():
        m = re.search(r'Студент (\d+) получил и сообщил билет (\d+)', line)
        if m:
            student, ticket = map(int, m.groups())
            assert student not in tickets and ticket not in tickets.values()
            tickets[student] = ticket
        m = re.search(r'Студент (\d+) передал работу; поставлен в очередь \((\d+)/(\d+)\)', line)
        if m:
            student, size, capacity = map(int, m.groups())
            assert student in tickets and student not in queue and student not in started
            queue.append(student)
            assert len(queue) == size and 1 <= size <= capacity
        m = re.search(r'Преподаватель (\d+) начал проверку работы студента (\d+)', line)
        if m:
            teacher, student = map(int, m.groups())
            assert teacher not in busy and student not in started
            assert queue.popleft() == student, 'FIFO violated'
            busy[teacher] = student
            started.add(student)
            maximum_busy = max(maximum_busy, len(busy))
        m = re.search(r'Преподаватель (\d+) закончил проверку студента (\d+); оценка (\d+)', line)
        if m:
            teacher, student, grade = map(int, m.groups())
            assert busy.pop(teacher) == student and student not in finished and 2 <= grade <= 5
            finished[student] = (teacher, grade)
        m = re.search(r'Студент (\d+) получил свою оценку (\d+)', line)
        if m:
            student, grade = map(int, m.groups())
            assert student not in received and finished[student][1] == grade
            received[student] = grade
    rows = re.findall(r'СТУДЕНТ id=(\d+) ticket=(\d+) state=(\w+) teacher=(\d+) grade=(\d+)', text)
    summary = re.search(r'ИТОГ reason=(\w+) admitted=(\d+) completed=(\d+) unfinished=(\d+) rejected=(\d+)', text)
    assert summary, text
    reason, admitted, completed, unfinished, rejected = summary.groups()
    admitted, completed, unfinished, rejected = map(int, (admitted, completed, unfinished, rejected))
    assert admitted == completed + unfinished and len(rows) == admitted + rejected
    assert completed == len(finished) == len(received)
    assert sum(r[2] == 'not_admitted' for r in rows) == rejected
    assert sum(r[2] == 'done' for r in rows) == completed
    for student, ticket, state, teacher, grade in rows:
        student, ticket, teacher, grade = map(int, (student, ticket, teacher, grade))
        if state == 'done':
            assert finished[student] == (teacher, grade)
        else:
            assert grade == 0
        if state == 'not_admitted':
            assert ticket == 0
    if reason == 'complete':
        assert unfinished == 0 and not busy and not queue
    return maximum_busy


def run(name, args=(), expected=0, reason='complete'):
    log = RESULTS / (name + '.log')
    p = subprocess.run([str(EXE), *map(str, args), '--log', str(log)],
                       capture_output=True, text=True, encoding='utf-8', timeout=20)
    assert p.returncode == expected, (name, p.returncode, p.stderr)
    if expected == 0:
        assert p.stdout == log.read_text(encoding='utf-8')
        validate(p.stdout)
        assert f'reason={reason}' in p.stdout
    passed.append(name)
    print('PASS', name)
    return p.stdout


normal = run('normal', ['--max-time', 0])
assert 'completed=6 unfinished=0' in normal
fixed = run('fixed', ['--teachers', 1, '--students', 8, '--tickets', 8,
    '--ticket-mode', 'sequential', '--grade-mode', 'ticket', '--prep-min', 1,
    '--prep-max', 1, '--queue', 1, '--check-time', 5, '--max-time', 0])
assert 'ожидает места' in fixed
for student in range(1, 9):
    assert f'Студент {student} получил свою оценку {2 + (student-1)%4}' in fixed
parallel = run('parallel', ['--teachers', 4, '--students', 20, '--tickets', 20,
    '--prep-min', 1, '--prep-max', 1, '--check-time', 20, '--max-time', 0])
assert validate(parallel) > 1, 'No overlapping checks observed'
run('shortage', ['--students', 5, '--tickets', 2, '--max-time', 0])
run('empty', ['--students', 0])
run('no_tickets', ['--tickets', 0])
run('timeout_preparation', ['--max-time', 1, '--prep-min', 100, '--prep-max', 100], reason='timeout')
run('timeout_waiters', ['--teachers', 1, '--students', 40, '--tickets', 40,
    '--queue', 1, '--prep-min', 1, '--prep-max', 1, '--check-time', 100,
    '--max-time', 20], reason='timeout')
run('zero_delay', ['--delay-ms', 0, '--max-time', 0])
for idx, args in enumerate([
    ['--teachers', 0], ['--queue', 0], ['--prep-min', 9, '--prep-max', 2],
    ['--students', 'abc'], ['--students', -1], ['--grade-mode', 'bad'],
    ['--seed', 0], ['--unknown', 1], ['--students', '999999999999999999999'],
    ['--students'], ['--ticket-mode', 'bad']]):
    run('invalid_' + str(idx), args, expected=2)
for seed in range(1, 11):
    run('stress_' + str(seed), ['--students', 60, '--tickets', 60, '--teachers', seed,
        '--queue', 1, '--seed', seed, '--max-time', 0, '--delay-ms', 1])
# Linux signals are intentionally tested on Linux; Windows terminate() bypasses C handlers.
if os.name == 'posix':
    for sig in (signal.SIGINT, signal.SIGTERM):
        name = 'signal_' + str(int(sig))
        log = RESULTS / (name + '.log')
        p = subprocess.Popen([str(EXE), '--teachers', '1', '--students', '30',
            '--tickets', '30', '--queue', '1', '--prep-min', '1', '--prep-max', '1',
            '--check-time', '1000', '--max-time', '0', '--log', str(log)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        # Wait for evidence that queue producers and consumers have started.
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if log.exists() and 'ожидает места' in log.read_text(encoding='utf-8'):
                break
            time.sleep(0.01)
        else:
            p.kill()
            raise AssertionError('Workers did not reach queue wait')
        p.send_signal(sig)
        out, err = p.communicate(timeout=5)
        assert p.returncode == 128 + int(sig), (p.returncode, err)
        assert 'reason=interrupted' in out and out == log.read_text(encoding='utf-8')
        validate(out)
        passed.append(name)
        print('PASS', name)
else:
    print('SKIP POSIX SIGINT/SIGTERM integration checks on Windows')
invalid_path = RESULTS / 'missing-parent' / 'exam.log'
p = subprocess.run([str(EXE), '--log', str(invalid_path)], capture_output=True, timeout=5)
assert p.returncode == 2
passed.append('invalid_log_path')
summary = f'{len(passed)} tests passed; platform={os.name}\n'
if os.name != 'posix':
    summary += 'Linux/WSL runtime and POSIX signal tests have not been executed here.\n'
(RESULTS / 'test-summary.txt').write_text(summary, encoding='utf-8')
print(summary)
