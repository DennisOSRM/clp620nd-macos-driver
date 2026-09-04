"""Tests for the clp620 CUPS backend.

Builds a test variant of the backend pointed at test/stub_socket.sh instead of
the real socket backend, and at a synthetic SNMP agent (test/fakeagent.py) on a
high port instead of the printer, so the jam-filtering logic can be exercised
including the cases a live printer will not produce on demand.
Run with `make check`.
"""
import os, subprocess, sys, time, signal, socket

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TMP  = os.environ.get("TESTDIR", os.path.join(HERE, "tmp"))
os.makedirs(TMP, exist_ok=True)

PORT = 11611
BE   = os.path.join(TMP, "clp620-test")
STUB = os.path.join(HERE, "stub_socket.sh")

fails = []
def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail and not cond else ""))
    if not cond: fails.append(name)

def build():
    cmd = ["cc", "-O2", "-Wall", "-Wextra",
           '-DSOCKET_BACKEND="%s"' % STUB, '-DSNMP_PORT="%d"' % PORT,
           "-o", BE, os.path.join(ROOT, "src", "clp620-backend.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); sys.exit(1)

class Agent:
    def __init__(self, scenario): self.scenario = scenario
    def __enter__(self):
        self.p = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "fakeagent.py"), self.scenario, str(PORT)],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for _ in range(100):                       # wait for "ready"
            line = self.p.stdout.readline()
            if line.strip() == "ready": break
        return self
    def __exit__(self, *a):
        self.p.terminate(); self.p.wait(timeout=5)

def states(err):
    """Only the STATE: lines - DEBUG lines quote reasons and must not count."""
    return [l.strip() for l in err.splitlines() if l.startswith("STATE:")]

def run(uri="clp620://127.0.0.1:9100", rc=None, stdin=b""):
    env = dict(os.environ, DEVICE_URI=uri)
    if rc is not None: env["STUB_RC"] = str(rc)
    p = subprocess.run([BE, "1", "dennis", "title", "1", ""],
                       input=stdin, capture_output=True, env=env)
    return p.returncode, p.stderr.decode()

build()

print("\n== no real jam: the firmware's bogus bit is dropped ==")
with Agent("nojam"):
    rc, err = run()
check("exit 0", rc == 0, str(rc))
check("jam-only line becomes an explicit clear", "STATE: -media-jam-warning" in err, err)
check("no STATE line still sets the jam",
      not any("+media-jam-warning" in l for l in states(err)), str(states(err)))
check("jam stripped from a mixed line, siblings kept",
      "STATE: +marker-supply-low-report" in err, err)
check("unrelated state passed through", "STATE: +toner-low-report" in err, err)
check("suppression is logged", "suppressed" in err, err)
check("non-STATE lines untouched", "PAGE: 1 1" in err, err)

print("\n== a real jam still reports ==")
for scen, why in [("jamcode", "prtAlertCode 8"), ("jamdesc", "description says jam")]:
    with Agent(scen):
        rc, err = run()
    check("%s -> jam passed through" % why,
          "STATE: +media-jam-warning" in states(err), str(states(err)))
    check("%s -> no false clear" % why,
          "STATE: -media-jam-warning" not in states(err), str(states(err)))
    check("%s -> nothing suppressed" % why, "suppressed" not in err, err)

print("\n== SNMP unreachable: fail safe, do not hide it ==")
rc, err = run()
check("jam passed through when SNMP is silent",
      "STATE: +media-jam-warning" in states(err), str(states(err)))
check("the fallback is logged", "SNMP did not answer" in err, err)

print("\n== URI handling ==")
with Agent("nojam"):
    rc, err = run()
check("child gets a rewritten socket:// URI",
      "DEVICE_URI=socket://127.0.0.1:9100" in err, err)
rc, err = run(uri="socket://127.0.0.1:9100")
check("non-clp620 URI is refused", rc != 0 and "is not clp620://" in err, "rc=%d %s" % (rc, err))

print("\n== exit status is propagated verbatim ==")
with Agent("nojam"):
    for want in (0, 1, 2, 4, 6, 7):
        rc, _ = run(rc=want)
        check("stub exit %d -> backend exit %d" % (want, want), rc == want, str(rc))

print("\n== discovery mode ==")
p = subprocess.run([BE], capture_output=True)
check("argc==1 exits 0 and lists nothing",
      p.returncode == 0 and not p.stdout.strip(),
      "rc=%d out=%r" % (p.returncode, p.stdout))

print()
if fails:
    print("%d FAILED: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
print("all backend tests passed")
