# Input robot for tools/soak.sh --input: plays hard on the keymapper through the control socket
# (usage: soak_robot.py <socket> <minutes>). Runs on the home screen, never in a real match.
import os, random, select, socket, sys, time
sock_path, minutes = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); s.setblocking(False)
def send(c):
    s.sendall((c + "\n").encode())
    while select.select([s], [], [], 0)[0]:
        if not s.recv(65536): break
W, A, S_, D, ALT = 17, 30, 31, 32, 56
TAPS = [57, 46, 44, 19, 33, 34, 35, 2, 3, 4, 42, 15, 45, 16, 18]   # space c z r f g h 1 2 3 shift tab x q e
UNMAPPED = [24, 39, 51, 52, 13, 12]                                    # o ; , . = -
random.seed(7)
send(f"keydown {ALT}"); send(f"keyup {ALT}")  # aim mode on
held_dirs, held_until = set(), 0.0
fire_until, tap_up = 0.0, {}
end = time.time() + minutes * 60
n = recenters = 0
while time.time() < end:
    now = time.time()
    if now > held_until:  # new WASD combo, sometimes none (stand still)
        new = set(random.sample([W, A, S_, D], random.choice([0, 1, 1, 2])))
        for k in held_dirs - new: send(f"keyup {k}")
        for k in new - held_dirs: send(f"keydown {k}")
        held_dirs, held_until = new, now + random.uniform(0.05, 2.0)
    # aim: mostly small moves, frequent big flicks
    dx = random.gauss(0, 6) + (random.choice([-1, 1]) * random.uniform(80, 400) if random.random() < 0.08 else 0)
    dy = random.gauss(0, 4) + (random.choice([-1, 1]) * random.uniform(40, 200) if random.random() < 0.05 else 0)
    send(f"mouse {int(dx)} {int(dy)}")
    if abs(dx) > 80: recenters += 1
    if fire_until and now > fire_until: send("click 0 0"); fire_until = 0
    elif not fire_until and random.random() < 0.02: send("click 0 1"); fire_until = now + random.uniform(0.05, 1.2)
    if random.random() < 0.01: send("click 1 1"); send("click 1 0")
    for k, t in list(tap_up.items()):
        if now > t: send(f"keyup {k}"); del tap_up[k]
    if random.random() < 0.03:
        k = random.choice(TAPS)
        if k not in tap_up: send(f"keydown {k}"); tap_up[k] = now + random.uniform(0.03, 0.4)
    if random.random() < 0.01:
        k = random.choice(UNMAPPED); send(f"keydown {k}"); send(f"keyup {k}")
    n += 1
    time.sleep(0.005)
# let go of everything
for k in held_dirs: send(f"keyup {k}")
for k in tap_up: send(f"keyup {k}")
send("click 0 0"); send(f"keydown {ALT}"); send(f"keyup {ALT}")  # aim mode off lifts aim + fire
print(f"sent {n} steps, ~{recenters} flicks")
