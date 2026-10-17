#!/usr/bin/env python3
"""Interactive tests: run mysh inside a pseudo-terminal so job control
(Ctrl-C, Ctrl-Z, fg, bg) behaves exactly as for a real user."""
import os, pty, select, sys, time, re, signal

SHELL = "./mysh"
passed = failed = 0

class Session:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(SHELL, [SHELL])
        self.buf = ""
        self.expect("mysh> ")

    def send(self, s):
        os.write(self.fd, s.encode())

    def expect(self, pattern, timeout=6):
        end = time.time() + timeout
        start = len(self.buf)
        while time.time() < end:
            if re.search(pattern, self.buf[start:]):
                return True
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096).decode(errors="replace")
                except OSError:
                    break
        return bool(re.search(pattern, self.buf[start:]))

    def cmd(self, line, timeout=6):
        self.send(line + "\n")
        return self.expect("mysh> ", timeout)

    def close(self):
        try:
            self.send("exit\n"); time.sleep(0.2)
            os.kill(self.pid, signal.SIGKILL)
        except OSError:
            pass
        try: os.waitpid(self.pid, 0)
        except OSError: pass

def result(name, ok, detail=""):
    global passed, failed
    print(("PASS  " if ok else "FAIL  ") + name)
    if ok: passed += 1
    else:
        failed += 1
        if detail: print("      " + detail)

def tail(s):  # output since last N chars, cleaned
    return s.replace("\r", "")

# P1: Ctrl-C kills the foreground job but not the shell
s = Session()
s.send("sleep 30\n"); time.sleep(0.5)
s.send("\x03")
ok = s.expect("mysh> ")
s.cmd("echo alive")
result("P1 Ctrl-C stops foreground job, shell survives", ok and "alive" in s.buf, tail(s.buf)[-200:])
s.close()

# P2: Ctrl-Z stops the foreground job and adds it to the job table
s = Session()
s.send("sleep 30\n"); time.sleep(0.5)
s.send("\x1a")
ok = s.expect(r"Stopped")
s.expect("mysh> ")
s.cmd("jobs")
result("P2 Ctrl-Z stops job, jobs shows Stopped", ok and s.buf.count("Stopped") >= 2, tail(s.buf)[-250:])
# P3: bg resumes it
s.cmd("bg %1")
s.cmd("jobs")
result("P3 bg resumes stopped job (Running)", "Running" in s.buf.split("bg %1")[-1], tail(s.buf)[-250:])
# P4: fg brings it back; Ctrl-C ends it
s.send("fg %1\n"); time.sleep(0.5)
s.send("\x03")
ok = s.expect("mysh> ")
s.cmd("jobs")
result("P4 fg returns job to foreground, Ctrl-C ends it", ok and "No jobs." in s.buf, tail(s.buf)[-250:])
s.close()

# P5: kill a *stopped* job (needs implicit SIGCONT)
s = Session()
s.send("sleep 30\n"); time.sleep(0.5); s.send("\x1a"); s.expect("mysh> ")
s.cmd("kill %1"); time.sleep(0.5)
s.cmd("jobs")
result("P5 kill works on a stopped job", "Terminated" in s.buf, tail(s.buf)[-250:])
s.close()

# P6: completion notice for a background job appears before next prompt
s = Session()
s.cmd("sleep 1 &")
time.sleep(1.5)
s.send("\n"); s.expect("mysh> ")
result("P6 background completion reported automatically", "Done" in s.buf, tail(s.buf)[-250:])
s.close()

# P7: background job that reads the terminal does not steal shell input
s = Session()
s.cmd("cat &")
time.sleep(0.5)
s.cmd("echo still-ok")
result("P7 background cat does not steal the terminal", "still-ok" in s.buf, tail(s.buf)[-250:])
s.cmd("kill %1")
s.close()

print("----\npassed: %d  failed: %d" % (passed, failed))
sys.exit(1 if failed else 0)
