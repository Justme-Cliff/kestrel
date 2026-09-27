import socket, sys, time, statistics
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile()
res = []
for i in range(int(sys.argv[2])):
    x, y = 500 + (i % 5) * 60, 250 + (i % 3) * 40
    s.sendall(f"latency {x} {y}\n".encode())
    line = ""
    while not line.strip(): line = f.readline()
    v = float(line.split()[-1]); res.append(v)
    time.sleep(0.35)
ok = [v for v in res if v >= 0]
print("n", len(res), "missed", len(res) - len(ok))
if ok: print("median %.1f  p90 %.1f  min %.1f  max %.1f" % (statistics.median(ok), sorted(ok)[int(len(ok)*0.9)-1], min(ok), max(ok)))
