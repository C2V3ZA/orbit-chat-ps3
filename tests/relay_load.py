"""Regression: a tunnel delivers every friend connection from 127.0.0.1. The relay must keep
answering far beyond 64 simultaneous connections from that one address."""
import json, socket, sys

port = int(sys.argv[1])
status_port = int(sys.argv[2]) if len(sys.argv) > 2 else port  # status only from the local side
held = []
for i in range(80):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    held.append(s)  # keep them all open, like long-lived chat and voice links
answered = 0
for s in held:
    s.sendall(b'ORBIT-RELAY 1 JOIN 0123456789abcdef\n')
for s in held:
    try:
        if s.recv(16) == b'OFFLINE\n':
            answered += 1
    except OSError:
        pass
print('answered', answered, 'of 80')
assert answered == 80, answered
for s in held:
    s.close()
q = socket.create_connection(('127.0.0.1', status_port), timeout=5)
q.sendall(b'ORBIT-RELAY 1 STATUS -\n')
status = json.loads(q.makefile().readline())
assert 'hosts' in status and 'pipes' in status
print('PASS: relay answers 80 simultaneous tunnel connections from 127.0.0.1; status query works')
