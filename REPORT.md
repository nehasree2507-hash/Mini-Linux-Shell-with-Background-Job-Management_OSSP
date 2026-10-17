---
title: "Mini Linux Shell with Background Job Management"
subtitle: "Project Report"
---

# 1. Abstract

This project implements **mysh**, a mini Linux shell in C that supports command execution, background jobs, job listing and process termination. It is built directly on POSIX system calls (`fork`, `execvp`, `waitpid`, `setpgid`, `tcsetpgrp`, `sigaction`, `kill`) and reproduces the core job-control behaviour of production shells: each job lives in its own process group, finished background jobs are reaped asynchronously through `SIGCHLD`, and Ctrl-C / Ctrl-Z affect only the foreground job. The system is validated by 33 automated tests (26 scripted, 7 run in a pseudo-terminal), all of which pass.

# 2. Problem Statement

Develop a shell that supports command execution, background jobs, job listing and process termination.

**Expected outcome:** a working interactive shell where a user can start programs, run them in the background with `&`, see what is running, and stop any of them, without the shell blocking, leaking zombie processes, or being killed by the user's own signals.

# 3. Objectives

1. Execute arbitrary external commands with arguments, quoting and I/O redirection.
2. Run commands in the background and keep the shell responsive.
3. Maintain a job table and display it on demand (`jobs`).
4. Terminate jobs by job ID or PID using any signal (`kill`).
5. Support foreground/background switching (`fg`, `bg`, Ctrl-Z).
6. Avoid zombie processes and race conditions.
7. Validate behaviour with repeatable tests.

# 4. Technologies Used

| Item | Purpose |
|---|---|
| C (GNU11) / gcc | Implementation language |
| POSIX process API: `fork`, `execvp`, `waitpid` | Process creation and supervision |
| Process groups and sessions: `setpgid`, `tcsetpgrp` | Job isolation, terminal ownership |
| Signals: `sigaction`, `sigprocmask`, `kill` | Asynchronous child tracking, job control |
| `dup2`, `open` | I/O redirection |
| Bash and Python 3 (`pty` module) | Test harnesses |
| Make, Git/GitHub | Build and version control |

# 5. System Design

## 5.1 Architecture

```
 input line
     |
     v
 tokenize + parse  ->  command_t
     |
     v
 built-in?  --yes-->  cd | jobs | fg | bg | kill | help | exit
     |
     no
     v
 fork + setpgid + execvp
     |
     +-----------------------+------------------------+
     | foreground                                     | background (&)
     v                                                v
 give terminal to the job                       add job to table
 waitpid(WUNTRACED)                             print "[n] pid"
 take terminal back                             return to prompt
     |                                                |
     |        SIGCHLD handler updates job state       |
     v                                                v
 reap_and_report(): print Done / Terminated before the next prompt
```

## 5.2 Main data structures

```c
typedef struct {
    pid_t     pid;        /* process-group id as well */
    job_state state;      /* FREE, RUNNING, STOPPED, DONE, KILLED */
    int       exit_code;  /* for DONE   */
    int       term_sig;   /* for KILLED */
    int       notified;
    char      cmd[1024];  /* text shown by `jobs` */
} job_t;
static job_t jobs[64];
```

The user-visible job ID is the table index plus one. A `command_t` holds the parsed `argv`, redirection targets and the background flag.

## 5.3 Job life cycle

| From | Event | To |
|---|---|---|
| (new) | `fork` + `exec` | RUNNING |
| RUNNING | process calls `exit` | DONE (exit code stored) |
| RUNNING | killed by a signal | KILLED (signal stored) |
| RUNNING | Ctrl-Z or `SIGSTOP` | STOPPED |
| STOPPED | `fg`, `bg` or `kill` (sends `SIGCONT`) | RUNNING |
| DONE / KILLED | reported once before a prompt | slot freed |

# 6. Implementation Approach

## 6.1 Parsing

A small tokenizer handles single and double quotes and treats unquoted `&`, `<`, `>` and `>>` as separate operator tokens, so `sleep 5&` works as expected. `&` is only legal as the last token; a missing redirection target or unterminated quote produces a clear error.

## 6.2 Command execution

`run_external` blocks `SIGCHLD`, calls `fork`, and in the child:

1. `setpgid(0,0)` — start a new process group;
2. give the terminal to the group if it is a foreground job;
3. restore default signal handlers (the shell itself ignores SIGINT/SIGTSTP);
4. apply redirections with `open` + `dup2`;
5. `execvp`; on failure print the error and `_exit(127)`.

The parent also calls `setpgid(pid,pid)`, closing the race over which process runs first.

## 6.3 Background jobs

For a command ending in `&` the parent registers the job in the table, prints `[id] pid` and returns to the prompt immediately. Background jobs without an explicit input redirect read from `/dev/null`, so they cannot compete with the shell for the keyboard.

## 6.4 Asynchronous completion tracking

The `SIGCHLD` handler loops over table entries and calls `waitpid(pid, WNOHANG | WUNTRACED | WCONTINUED)`, then records *exited*, *signalled*, *stopped* or *continued*. It only modifies memory (async-signal-safe); the message is printed by `reap_and_report()` just before the next prompt. Because every background child is reaped as soon as it changes state, **no zombies** accumulate.

## 6.5 Foreground jobs and the terminal

The shell hands the terminal to the job with `tcsetpgrp`, waits with `waitpid(WUNTRACED)`, and takes the terminal back afterwards. Ctrl-C and Ctrl-Z are delivered by the terminal driver to the *foreground process group only*. A foreground job that is stopped is inserted into the job table as `Stopped`.

## 6.6 Race-condition control

`SIGCHLD` is blocked from before `fork` until the job is in the table (and during foreground waits). Without this, a short-lived child could exit before being registered, or the handler could steal the exit status the foreground `waitpid` is waiting for.

## 6.7 Termination

`kill` accepts `%n` (job) or a raw PID, and a signal number or name (`-9`, `-KILL`, `-SIGTERM`). For jobs the signal goes to the entire process group (`kill(-pgid, sig)`). If the job is stopped, `SIGCONT` follows so the process can act on the signal. On `exit` or Ctrl-D the shell sends `SIGTERM` to all remaining jobs so nothing is left orphaned.

# 7. Testing and Validation

## 7.1 Method

Two suites, run with `make test`:

* `tests/run_tests.sh` — feeds scripted sessions through a pipe and checks output.
* `tests/pty_tests.py` — spawns `mysh` in a pseudo-terminal and sends real keystrokes (Ctrl-C = `0x03`, Ctrl-Z = `0x1A`), because job control only exists on a terminal.

All test commands are `sleep`, `echo`, `cat`, `sh -c "exit N"`; tests signal only processes created by `mysh`.

## 7.2 Test cases and results

| ID | Scenario | Expected | Result |
|---|---|---|---|
| T01 | `echo hello` | prints `hello` | Pass |
| T02 | multiple arguments | `a b c` | Pass |
| T03 | quoted argument with spaces | spaces preserved | Pass |
| T04 | unknown command | "No such file or directory" | Pass |
| T05 | `>` redirection | file contains data | Pass |
| T06 | `>>` redirection | data appended | Pass |
| T07 | `<` redirection | input read from file | Pass |
| T08 | `cd /tmp; pwd` | `/tmp` | Pass |
| T09 | `cd` to bad directory | error message | Pass |
| T10 | `sleep 5 &` | `[1] <pid>` printed | Pass |
| T11 | `jobs` with a running job | state `Running` | Pass |
| T12 | `jobs` with none | `No jobs.` | Pass |
| T13 | short job finishes | reported `Done` | Pass |
| T14 | `kill %1` | `Terminated` | Pass |
| T15 | `kill -KILL %1` | `Killed` | Pass |
| T16 | job list shows command text | `sleep 30` visible | Pass |
| T17 | `kill %9` (no such job) | error | Pass |
| T18 | `kill -FOO` | "invalid signal" | Pass |
| T19 | several background jobs | IDs `[1]`, `[2]` | Pass |
| T20 | job exits with status 3 | `Exit 3` | Pass |
| T21 | `echo a & b` | syntax error | Pass |
| T22 | unterminated quote | error | Pass |
| T23 | `help` | built-in list | Pass |
| T24 | shell responsive while job runs | next command executes | Pass |
| T25 | finished jobs leave no zombies | no `Z` state | Pass |
| T26 | `exit` with a running job | job terminated | Pass |
| P1 | Ctrl-C during foreground job | job dies, shell survives | Pass |
| P2 | Ctrl-Z | job shown `Stopped` | Pass |
| P3 | `bg %1` | job `Running` again | Pass |
| P4 | `fg %1` then Ctrl-C | job ends, `No jobs.` | Pass |
| P5 | `kill` a stopped job | `Terminated` | Pass |
| P6 | background job completes at idle prompt | `Done` reported | Pass |
| P7 | `cat &` does not steal keyboard | shell still reads input | Pass |

**Summary: 33 / 33 tests passed**, compiled with `-Wall -Wextra` and no warnings.

# 8. Results

Captured output from a real session:

```
mysh> echo Hello from mysh
Hello from mysh
mysh> sleep 60 &
[1] 564
mysh> sleep 2 &
[2] 565
mysh> sleep 100 > /dev/null &
[3] 566
mysh> jobs
[1] 564      Running            sleep 60
[2] 565      Running            sleep 2
[3] 566      Running            sleep 100 > /dev/null
mysh> kill %1
sent Terminated to [1] 564
[1] 564      Terminated         sleep 60
mysh> sleep 3
[2] 565      Done               sleep 2
mysh> kill -KILL %3
sent Killed to [3] 566
[3] 566      Killed             sleep 100 > /dev/null
mysh> jobs
No jobs.
```

The results show: the shell stays responsive while jobs run; completion and termination are reported automatically; signals reach the right processes; and no zombie or orphaned processes remain.

# 9. Impact and Learning Outcomes

* Concrete understanding of how real shells implement job control, which underlies terminals, CI runners, container entry-points and process supervisors.
* Hands-on experience with process groups, sessions, controlling terminals and asynchronous signals, including the race conditions they introduce.
* A tested, documented reference implementation that can be extended (pipes, history, scripting).

# 10. Responsible Implementation

* The shell and tests were developed and run in a controlled environment using only harmless programs (`sleep`, `echo`, `cat`).
* `kill` is a convenience over the system call and is subject to normal OS permissions; the tests signal only children of `mysh`.
* Jobs are bounded (64), lines are bounded (1023 characters), and all remaining jobs are terminated on exit so the shell leaves nothing running.
* The implementation uses only POSIX interfaces documented in the man pages; sources consulted are listed in section 12.

# 11. Limitations and Future Work

* Pipelines (`|`), command lists (`;`, `&&`, `||`), globbing, variable expansion and history are not implemented.
* Pipes could be added by creating one `pipe()` per stage, wiring ends with `dup2`, and putting all stages in one process group; the job table would then track the group rather than a single PID.
* Further possibilities: `disown`, `wait`, `jobs -l`, `%+`/`%-` job specifiers, a configuration file, and tab completion.

# 12. References

1. W. R. Stevens and S. A. Rago, *Advanced Programming in the UNIX Environment*, chapters on process control, signals and terminal I/O.
2. R. E. Bryant and D. R. O'Hallaron, *Computer Systems: A Programmer's Perspective*, chapter 8 (Exceptional Control Flow).
3. Linux man pages: `fork(2)`, `execvp(3)`, `waitpid(2)`, `setpgid(2)`, `tcsetpgrp(3)`, `sigaction(2)`, `signal-safety(7)`, `kill(2)`.
4. GNU C Library manual, "Job Control" chapter (implementing a shell).

# 13. Team Contributions

| Member | Role | Contribution |
|---|---|---|
| _Name 1_ | | |
| _Name 2_ | | |
| _Name 3_ | | |
| _Name 4_ | | |
