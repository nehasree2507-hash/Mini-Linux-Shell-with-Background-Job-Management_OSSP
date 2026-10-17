/*
 * mysh - a mini Linux shell with background job management
 *
 * Features:
 *   - Runs external commands (fork/execvp) with quoting and < > >> redirection
 *   - Background jobs with trailing '&'
 *   - Job table: jobs, fg, bg, kill
 *   - Async job completion tracking via SIGCHLD (no zombies)
 *   - Each job runs in its own process group; Ctrl-C / Ctrl-Z affect only
 *     the foreground job, never the shell
 *
 * Build:  gcc -Wall -Wextra -O2 -o mysh mysh.c
 * Run:    ./mysh
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 128
#define MAX_JOBS 64

typedef enum { J_FREE = 0, J_RUNNING, J_STOPPED, J_DONE, J_KILLED } job_state;

typedef struct {
    pid_t pid;              /* also the process group id */
    job_state state;
    int exit_code;          /* valid for J_DONE   */
    int term_sig;           /* valid for J_KILLED */
    int notified;           /* completion already reported to the user? */
    char cmd[MAX_LINE];
} job_t;

static job_t jobs[MAX_JOBS];
static pid_t shell_pgid;
static int interactive;
static sigset_t chld_set;

/* ---------------------------------------------------------------- jobs */

static int add_job(pid_t pid, const char *cmd, job_state st) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_FREE) {
            memset(&jobs[i], 0, sizeof jobs[i]);
            jobs[i].pid = pid;
            jobs[i].state = st;
            snprintf(jobs[i].cmd, sizeof jobs[i].cmd, "%s", cmd);
            return i;
        }
    }
    return -1;
}

static int find_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].state != J_FREE && jobs[i].pid == pid) return i;
    return -1;
}

/* Job IDs shown to the user are slot index + 1. */
static int find_by_id(int id) {
    if (id < 1 || id > MAX_JOBS || jobs[id - 1].state == J_FREE) return -1;
    return id - 1;
}

static const char *state_name(job_state s) {
    switch (s) {
    case J_RUNNING: return "Running";
    case J_STOPPED: return "Stopped";
    case J_DONE:    return "Done";
    case J_KILLED:  return "Terminated";
    default:        return "?";
    }
}

/*
 * SIGCHLD handler: reap state changes of background/stopped jobs only.
 * Only touches the job table (async-signal-safe); printing happens later.
 * The foreground process is waited on explicitly with SIGCHLD blocked.
 */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved = errno;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state != J_RUNNING && jobs[i].state != J_STOPPED) continue;
        int st;
        pid_t r = waitpid(jobs[i].pid, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (r <= 0) continue;
        if (WIFEXITED(st)) {
            jobs[i].state = J_DONE;
            jobs[i].exit_code = WEXITSTATUS(st);
        } else if (WIFSIGNALED(st)) {
            jobs[i].state = J_KILLED;
            jobs[i].term_sig = WTERMSIG(st);
        } else if (WIFSTOPPED(st)) {
            jobs[i].state = J_STOPPED;
        } else if (WIFCONTINUED(st)) {
            jobs[i].state = J_RUNNING;
        }
    }
    errno = saved;
}

static void print_job(int i) {
    job_t *j = &jobs[i];
    char status[64];
    if (j->state == J_DONE && j->exit_code != 0)
        snprintf(status, sizeof status, "Exit %d", j->exit_code);
    else if (j->state == J_KILLED)
        snprintf(status, sizeof status, "%s", strsignal(j->term_sig));
    else
        snprintf(status, sizeof status, "%s", state_name(j->state));
    printf("[%d] %-8d %-18s %s\n", i + 1, (int)j->pid, status, j->cmd);
}

/* Report (once) and release finished jobs. Called before each prompt. */
static void reap_and_report(void) {
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);
    for (int i = 0; i < MAX_JOBS; i++) {
        if ((jobs[i].state == J_DONE || jobs[i].state == J_KILLED) && !jobs[i].notified) {
            print_job(i);
            jobs[i].state = J_FREE;
        }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* ------------------------------------------------------------- parsing */

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    int background;
    char *in_file, *out_file;
    int append;
    char display[MAX_LINE];  /* command text shown in the job list */
} command_t;

/*
 * Tokenize `line` into `buf`. Understands '...' and "..." quoting and
 * treats unquoted & < > >> as separate operator tokens.
 */
static int tokenize(const char *s, char *buf, char **tok, int *isop) {
    int n = 0;
    char *w = buf;
    while (*s) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (n >= MAX_ARGS - 1) return -1;
        tok[n] = w;
        if (*s == '&' || *s == '<' || *s == '>') {
            isop[n] = 1;
            *w++ = *s++;
            if (w[-1] == '>' && *s == '>') *w++ = *s++;
        } else {
            isop[n] = 0;
            char quote = 0;
            while (*s && (quote || (!isspace((unsigned char)*s) &&
                                    *s != '&' && *s != '<' && *s != '>'))) {
                if (quote) {
                    if (*s == quote) quote = 0; else *w++ = *s;
                } else if (*s == '\'' || *s == '"') {
                    quote = *s;
                } else {
                    *w++ = *s;
                }
                s++;
            }
            if (quote) return -2; /* unterminated quote */
        }
        *w++ = '\0';
        n++;
    }
    return n;
}

static int parse(const char *line, command_t *c, char *buf) {
    char *tok[MAX_ARGS];
    int isop[MAX_ARGS];
    memset(c, 0, sizeof *c);

    int n = tokenize(line, buf, tok, isop);
    if (n == -2) { fprintf(stderr, "mysh: unterminated quote\n"); return -1; }
    if (n < 0)   { fprintf(stderr, "mysh: too many arguments\n");  return -1; }

    for (int i = 0; i < n; i++) {
        if (isop[i] && !strcmp(tok[i], "&")) {
            if (i != n - 1) { fprintf(stderr, "mysh: '&' must end the command\n"); return -1; }
            c->background = 1;
        } else if (isop[i]) {
            if (i + 1 >= n || isop[i + 1]) {
                fprintf(stderr, "mysh: missing file after '%s'\n", tok[i]);
                return -1;
            }
            if (tok[i][0] == '<') c->in_file = tok[++i];
            else { c->append = tok[i][1] == '>'; c->out_file = tok[++i]; }
        } else {
            c->argv[c->argc++] = tok[i];
        }
    }
    c->argv[c->argc] = NULL;

    /* Display text: the line minus trailing '&' and whitespace. */
    snprintf(c->display, sizeof c->display, "%s", line);
    size_t len = strlen(c->display);
    while (len && (isspace((unsigned char)c->display[len - 1]) || c->display[len - 1] == '&'))
        c->display[--len] = '\0';
    return 0;
}

/* ----------------------------------------------------- foreground wait */

/* Caller must have SIGCHLD blocked. */
static void wait_foreground(pid_t pid, const char *cmd, int jidx) {
    int st;
    pid_t r;
    do { r = waitpid(pid, &st, WUNTRACED); } while (r < 0 && errno == EINTR);

    if (interactive) tcsetpgrp(STDIN_FILENO, shell_pgid);   /* take terminal back */

    if (r < 0) {
        if (jidx >= 0) jobs[jidx].state = J_FREE;
        return;
    }
    if (WIFSTOPPED(st)) {                                   /* Ctrl-Z */
        if (jidx < 0) jidx = add_job(pid, cmd, J_STOPPED);
        else jobs[jidx].state = J_STOPPED;
        if (jidx >= 0) { printf("\n"); print_job(jidx); }
        else kill(-pid, SIGKILL);                           /* job table full */
        return;
    }
    if (WIFSIGNALED(st)) {
        if (WTERMSIG(st) == SIGINT) printf("\n");
        else fprintf(stderr, "%s\n", strsignal(WTERMSIG(st)));
    }
    if (jidx >= 0) jobs[jidx].state = J_FREE;
}

/* ------------------------------------------------------- run a command */

static void run_external(command_t *c) {
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);   /* no handler until job is registered */

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        sigprocmask(SIG_SETMASK, &old, NULL);
        return;
    }

    if (pid == 0) {                            /* ---- child ---- */
        setpgid(0, 0);                         /* own process group */
        if (!c->background && interactive) tcsetpgrp(STDIN_FILENO, getpid());

        signal(SIGINT, SIG_DFL);  signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL); signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL); signal(SIGCHLD, SIG_DFL);
        sigprocmask(SIG_SETMASK, &old, NULL);

        if (c->background && !c->in_file) {    /* don't fight the shell for the tty */
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        }
        if (c->in_file) {
            int fd = open(c->in_file, O_RDONLY);
            if (fd < 0) { perror(c->in_file); _exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (c->out_file) {
            int fd = open(c->out_file,
                          O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
            if (fd < 0) { perror(c->out_file); _exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }
        execvp(c->argv[0], c->argv);
        fprintf(stderr, "mysh: %s: %s\n", c->argv[0], strerror(errno));
        _exit(127);
    }

    /* ---- parent ---- */
    setpgid(pid, pid);                         /* avoid the fork/exec race */

    if (c->background) {
        int j = add_job(pid, c->display, J_RUNNING);
        if (j < 0) fprintf(stderr, "mysh: job table full\n");
        else printf("[%d] %d\n", j + 1, (int)pid);
    } else {
        if (interactive) tcsetpgrp(STDIN_FILENO, pid);
        wait_foreground(pid, c->display, -1);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* ------------------------------------------------------------ builtins */

/* Parse "%N" or a bare PID into a job index; -1 if none. */
static int resolve_job(const char *arg) {
    if (arg[0] == '%') return find_by_id(atoi(arg + 1));
    char *end;
    long v = strtol(arg, &end, 10);
    if (*end || v <= 0) return -1;
    return find_by_pid((pid_t)v);
}

static void builtin_jobs(void) {
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);
    int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_FREE) continue;
        print_job(i);
        any = 1;
        if (jobs[i].state == J_DONE || jobs[i].state == J_KILLED) jobs[i].state = J_FREE;
    }
    if (!any) printf("No jobs.\n");
    sigprocmask(SIG_SETMASK, &old, NULL);
}

static void builtin_fg_bg(command_t *c, int fg) {
    const char *name = fg ? "fg" : "bg";
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);

    int idx = -1;
    if (c->argc >= 2) idx = resolve_job(c->argv[1]);
    else {                                           /* default: most recent job */
        for (int i = MAX_JOBS - 1; i >= 0; i--)
            if (jobs[i].state == J_RUNNING || jobs[i].state == J_STOPPED) { idx = i; break; }
    }
    if (idx < 0 || (jobs[idx].state != J_RUNNING && jobs[idx].state != J_STOPPED)) {
        fprintf(stderr, "mysh: %s: no such job\n", name);
    } else {
        job_t *j = &jobs[idx];
        printf("%s\n", j->cmd);
        if (fg) {
            if (interactive) tcsetpgrp(STDIN_FILENO, j->pid);
            j->state = J_RUNNING;
            kill(-j->pid, SIGCONT);
            wait_foreground(j->pid, j->cmd, idx);
        } else {
            j->state = J_RUNNING;
            kill(-j->pid, SIGCONT);
        }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

static int parse_signal(const char *s) {
    static const struct { const char *n; int v; } tbl[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
        {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"TERM", SIGTERM},
        {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP},
    };
    if (isdigit((unsigned char)s[0])) return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (!strcmp(s, tbl[i].n)) return tbl[i].v;
    return -1;
}

static void builtin_kill(command_t *c) {
    int sig = SIGTERM, first = 1;
    if (c->argc >= 2 && c->argv[1][0] == '-') {
        sig = parse_signal(c->argv[1] + 1);
        if (sig <= 0) { fprintf(stderr, "kill: invalid signal '%s'\n", c->argv[1] + 1); return; }
        first = 2;
    }
    if (first >= c->argc) {
        fprintf(stderr, "usage: kill [-SIGNAL] %%jobid|pid ...\n");
        return;
    }
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);
    for (int i = first; i < c->argc; i++) {
        int idx = resolve_job(c->argv[i]);
        if (idx >= 0) {
            kill(-jobs[idx].pid, sig);            /* whole process group */
            /* A stopped process can't act on SIGTERM etc. until resumed. */
            if (jobs[idx].state == J_STOPPED && sig != SIGSTOP && sig != SIGTSTP && sig != SIGCONT)
                kill(-jobs[idx].pid, SIGCONT);
            printf("sent %s to [%d] %d\n", strsignal(sig), idx + 1, (int)jobs[idx].pid);
        } else if (c->argv[i][0] != '%' && atoi(c->argv[i]) > 0) {
            if (kill((pid_t)atoi(c->argv[i]), sig) < 0) perror("kill");
        } else {
            fprintf(stderr, "kill: %s: no such job\n", c->argv[i]);
        }
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

static void builtin_help(void) {
    puts("mysh built-ins:\n"
         "  cmd [args] [< in] [> out | >> out]   run a command in the foreground\n"
         "  cmd ... &                            run a command in the background\n"
         "  jobs                                 list jobs\n"
         "  fg [%n]                              bring a job to the foreground\n"
         "  bg [%n]                              resume a stopped job in the background\n"
         "  kill [-SIG] %n | pid ...             send a signal (default TERM)\n"
         "  cd [dir]                             change directory\n"
         "  help                                 show this help\n"
         "  exit                                 quit (terminates remaining jobs)\n"
         "Ctrl-C interrupts and Ctrl-Z stops the foreground job.");
}

/* Returns 1 if `c` was a built-in. Sets *quit for exit. */
static int run_builtin(command_t *c, int *quit) {
    const char *cmd = c->argv[0];
    if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) { *quit = 1; return 1; }
    if (!strcmp(cmd, "jobs")) { builtin_jobs(); return 1; }
    if (!strcmp(cmd, "fg"))   { builtin_fg_bg(c, 1); return 1; }
    if (!strcmp(cmd, "bg"))   { builtin_fg_bg(c, 0); return 1; }
    if (!strcmp(cmd, "kill")) { builtin_kill(c); return 1; }
    if (!strcmp(cmd, "help")) { builtin_help(); return 1; }
    if (!strcmp(cmd, "cd")) {
        const char *dir = c->argc > 1 ? c->argv[1] : getenv("HOME");
        if (!dir || chdir(dir) < 0) perror("cd");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- main */

static void terminate_all_jobs(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_RUNNING || jobs[i].state == J_STOPPED) {
            kill(-jobs[i].pid, SIGTERM);
            kill(-jobs[i].pid, SIGCONT);
        }
    }
}

int main(void) {
    interactive = isatty(STDIN_FILENO);

    sigemptyset(&chld_set);
    sigaddset(&chld_set, SIGCHLD);

    if (interactive) {
        /* Wait until we are in the foreground, then take our own group. */
        while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);
        signal(SIGINT, SIG_IGN);  signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN); signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        shell_pgid = getpid();
        setpgid(shell_pgid, shell_pgid);
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    } else {
        shell_pgid = getpgrp();
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    char line[MAX_LINE], buf[2 * MAX_LINE + MAX_ARGS];
    int quit = 0;

    while (!quit) {
        reap_and_report();
        if (interactive) { printf("mysh> "); fflush(stdout); }

        if (!fgets(line, sizeof line, stdin)) {
            if (interactive) putchar('\n');
            break;                                   /* EOF / Ctrl-D */
        }
        if (!strchr(line, '\n') && !feof(stdin)) {   /* over-long line: discard rest */
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            fprintf(stderr, "mysh: line too long\n");
            continue;
        }

        command_t c;
        if (parse(line, &c, buf) < 0 || c.argc == 0) continue;
        if (!run_builtin(&c, &quit)) run_external(&c);
        fflush(stdout);
    }

    terminate_all_jobs();
    return 0;
}
