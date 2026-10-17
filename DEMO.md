# Live Demonstration Script (≈ 6 minutes)

Run in a normal terminal (not an IDE console — job control needs a real TTY).
All commands use harmless `sleep`/`echo`, so the demo cannot disturb other users or processes.

| # | Type | Expected result | Concept to explain |
|---|---|---|---|
| 1 | `make && ./mysh` | `mysh>` prompt | Build, zero warnings |
| 2 | `help` | built-in list | Feature overview |
| 3 | `echo "Hello   from mysh"` | spacing preserved | Tokenizer + quoting |
| 4 | `echo saved > /tmp/d.txt` then `cat /tmp/d.txt` | `saved` | `dup2` redirection |
| 5 | `sleep 60 &` | `[1] <pid>` | fork + exec, no wait for `&` |
| 6 | `sleep 5 &` then `jobs` | two Running jobs | Job table |
| 7 | `echo shell-is-responsive` | prints immediately | Background really is background |
| 8 | `kill %1` | `[1] … Terminated sleep 60` | Group signal + SIGCHLD update |
| 9 | wait 5 s, press Enter | `[2] … Done sleep 5` | Async completion, no zombies |
| 10 | `sleep 100` then **Ctrl-Z** | `Stopped` | WUNTRACED, job added to table |
| 11 | `bg %1`, then `jobs` | `Running` | SIGCONT to process group |
| 12 | `fg %1`, then **Ctrl-C** | job dies, shell survives | `tcsetpgrp`, signal isolation |
| 13 | `sleep 100 &` `kill -KILL %1` `jobs` | `Killed`, then `No jobs.` | Named signals |
| 14 | `kill %9`, `nosuchcmd`, `kill -FOO %1` | clear error messages | Error handling |
| 15 | In another terminal: `ps -o pid,pgid,stat,cmd --forest` | each job has its own PGID; no `Z` | Process groups |
| 16 | `exit` | no leftover `sleep` (`pgrep sleep`) | Cleanup on exit |
| 17 | `make test` | 26/26 and 7/7 pass | Validation |

## Questions to be ready for

- **Why process groups?** So Ctrl-C/Ctrl-Z reach only the foreground job and `kill %n` hits the whole job.
- **Why block SIGCHLD around `fork`?** A fast child could exit before it is in the job table; the handler would miss it.
- **Why does the handler not `printf`?** `printf` is not async-signal-safe; the handler only edits the table.
- **Why does `kill` send SIGCONT to stopped jobs?** A stopped process cannot act on SIGTERM until resumed.
- **Why `setpgid` in both parent and child?** Whichever runs first wins the race; both calls are harmless.
- **How would you add pipes?** `pipe()` per stage, `dup2` ends, all stages in one process group.
