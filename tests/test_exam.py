
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
EXE = str(ROOT / 'exam')
RESULTS = ROOT / 'results'
RESULTS.mkdir(exist_ok=True)
count = 0

def run(name, args=(), expected=0):
    global count
    log = RESULTS / (name + '.log')
    p = subprocess.run([EXE, *map(str, args), '--log', str(log)], capture_output=True, text=True, timeout=15)
    assert p.returncode == expected, (name, p.returncode, p.stderr)
    if expected == 0:
        assert p.stdout == log.read_text(), name
        validate(p.stdout)
    count += 1
    print('PASS', name)
    return p.stdout

def validate(text):
    starts = re.findall(r'Преподаватель (\d+) начал проверку работы студента (\d+)', text)
    ids = [int(s) for _, s in starts]
    assert len(ids) == len(set(ids)), 'Повторная проверка'
    tickets = re.findall(r'получил и сообщил билет (\d+)', text)
    assert len(tickets) == len(set(tickets)), 'Повторный билет'
    busy = {}
    for line in text.splitlines():
        t = int(re.search(r't=(\d+)', line)[1])
        start = re.search(r'Преподаватель (\d+) начал проверку работы студента (\d+)', line)
        finish = re.search(r'Преподаватель (\d+) закончил проверку студента (\d+); оценка (\d+)', line)
        if start:
            teacher, student = map(int, start.groups())
            assert teacher not in busy
            busy[teacher] = student
        if finish:
            teacher, student, grade = map(int, finish.groups())
            assert busy.pop(teacher) == student and 2 <= grade <= 5
        queue = re.search(r'поставлен в очередь \((\d+)/(\d+)\)', line)
        if queue: assert int(queue[1]) <= int(queue[2])
    rows = re.findall(r'СТУДЕНТ id=(\d+) ticket=(\d+) state=(\w+) teacher=(\d+) grade=(\d+)', text)
    summary = re.search(r'ИТОГ reason=(\w+) admitted=(\d+) completed=(\d+) unfinished=(\d+) rejected=(\d+)', text)
    reason, admitted, completed, unfinished, rejected = summary.groups()
    assert int(admitted) == int(completed) + int(unfinished)
    assert len(rows) == int(admitted) + int(rejected)
    assert sum(r[2] == 'done' for r in rows) == int(completed)
    assert sum(r[2] == 'not_admitted' for r in rows) == int(rejected)
    for student, ticket, state, teacher, grade in rows:
        if state == 'done':
            assert f'Студент {student} получил свою оценку {grade}' in text
        else: assert grade == '0'
    if reason == 'complete': assert not busy and int(unfinished) == 0

normal = run('normal')
assert 'completed=6 unfinished=0' in normal
repeat = run('repeat')
assert normal == repeat
fixed = run('fixed', ['--teachers', 1, '--students', 3, '--tickets', 3, '--ticket-mode', 'sequential', '--grade-mode', 'ticket', '--prep-min', 1, '--prep-max', 1, '--queue', 1, '--check-time', 2])
assert '[t=7] ИТОГ reason=complete' in fixed
assert 'waited=1' in fixed
for student, grade in [(1,2),(2,3),(3,4)]:
    assert f'Студент {student} получил свою оценку {grade}' in fixed
short = run('shortage', ['--students', 5, '--tickets', 2])
assert 'admitted=2 completed=2 unfinished=0 rejected=3' in short
empty = run('empty', ['--students', 0])
assert '[t=0] ИТОГ reason=complete' in empty
none = run('no_tickets', ['--tickets', 0])
assert 'rejected=6' in none
limit = run('timeout', ['--max-time', 1])
assert 'reason=timeout admitted=6 completed=0 unfinished=6' in limit
edge = run('boundary', ['--students', 1, '--tickets', 1, '--prep-min', 1, '--prep-max', 1, '--check-time', 2, '--max-time', 3])
assert '[t=3] ИТОГ reason=complete' in edge
unlimited = run('unlimited', ['--max-time', 0])
assert 'reason=complete' in unlimited
for idx, args in enumerate([
    ['--teachers',0], ['--queue',0], ['--prep-min',9,'--prep-max',2],
    ['--students','abc'], ['--students',-1], ['--grade-mode','bad'],
    ['--seed',0], ['--unknown',1], ['--students','999999999999999999999']]):
    run('invalid_' + str(idx), args, 2)
# Разнообразные нагрузки: узкая очередь, много преподавателей, разные seed.
for seed in range(1, 11):
    run('stress_' + str(seed), ['--students', 40, '--tickets', 40, '--teachers', seed, '--queue', 1, '--seed', seed, '--max-time', 0])
# Сигнал посылается только после первого события: обработчик уже установлен.
for sig in (signal.SIGINT, signal.SIGTERM):
    name = 'signal_' + str(sig.value)
    log = RESULTS / (name + '.log')
    p = subprocess.Popen([EXE, '--max-time','0','--delay-ms','200','--log',str(log)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    first = p.stdout.readline()
    assert 'НАЧАЛО' in first
    p.send_signal(sig)
    stdout, stderr = p.communicate(timeout=5)
    assert p.returncode == 128 + sig.value, stderr
    text = first + stdout
    assert 'reason=interrupted' in text
    validate(text)
    count += 1
    print('PASS', name)
print(f'ALL {count} TESTS PASSED')
