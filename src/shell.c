/*
 * lssh - LS-OpenServer Shell
 * FreeBSD 14
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o lssh lssh.c
 *
 * Features:
 *   - Command history (up/down arrows, !n recall)
 *   - Pipes (cmd1 | cmd2 | cmd3 ...)
 *   - I/O redirection (>, >>, <)
 *   - Environment variable expansion ($VAR, $?)
 *   - Background jobs (&) with job table
 *   - Built-ins: cd, exit, echo, export, unset, env,
 *                history, jobs, fg, kill, pwd, help
 *   - $? tracks last exit status
 *   - Ctrl-C kills foreground child, not the shell
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ─────────────────────────── tunables ─────────────────────────── */
#define MAX_CMD_LEN 4096
#define MAX_ARGS 128
#define MAX_PIPELINE 16 /* max stages in a pipe             */
#define MAX_HISTORY 100
#define MAX_JOBS 32
#define PROMPT_MAX 256

/* ─────────────────────────── history ──────────────────────────── */
static char *history[MAX_HISTORY];
static int history_count = 0;

static void history_add(const char *line) {
  if (strlen(line) == 0)
    return;
  /* Avoid duplicate consecutive entries */
  if (history_count > 0 && strcmp(history[history_count - 1], line) == 0)
    return;
  if (history_count == MAX_HISTORY) {
    free(history[0]);
    memmove(&history[0], &history[1], (MAX_HISTORY - 1) * sizeof(char *));
    history_count--;
  }
  history[history_count++] = strdup(line);
}

static void history_print(void) {
  for (int i = 0; i < history_count; i++)
    printf("  %3d  %s\n", i + 1, history[i]);
}

/* Expand !n — returns pointer to static buffer, or NULL on error */
static const char *history_expand(const char *line) {
  if (line[0] != '!')
    return line;
  if (line[1] == '!') {
    /* !! = last command */
    if (history_count == 0) {
      fprintf(stderr, "lssh: !!: no history\n");
      return NULL;
    }
    return history[history_count - 1];
  }
  int n = atoi(&line[1]);
  if (n < 1 || n > history_count) {
    fprintf(stderr, "lssh: !%d: event not found\n", n);
    return NULL;
  }
  return history[n - 1];
}

/* ─────────────────────────── job table ─────────────────────────── */
typedef enum { JOB_RUNNING, JOB_DONE, JOB_STOPPED } JobStatus;

typedef struct {
  int id;
  pid_t pid;
  JobStatus status;
  int exit_code;
  char cmdline[256];
} Job;

static Job jobs[MAX_JOBS];
static int job_count = 0;
static int next_job_id = 1;

static Job *job_add(pid_t pid, const char *cmdline) {
  if (job_count >= MAX_JOBS)
    return NULL;
  Job *j = &jobs[job_count++];
  j->id = next_job_id++;
  j->pid = pid;
  j->status = JOB_RUNNING;
  j->exit_code = -1;
  strncpy(j->cmdline, cmdline, sizeof(j->cmdline) - 1);
  return j;
}

static Job *job_by_pid(pid_t pid) {
  for (int i = 0; i < job_count; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

static Job *job_by_id(int id) {
  for (int i = 0; i < job_count; i++)
    if (jobs[i].id == id)
      return &jobs[i];
  return NULL;
}

/* Reap finished background jobs without blocking */
static void jobs_reap(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    Job *j = job_by_pid(pid);
    if (j) {
      j->status = JOB_DONE;
      j->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      printf("[%d] Done\t\t%s\n", j->id, j->cmdline);
    }
  }
}

static void jobs_print(void) {
  for (int i = 0; i < job_count; i++) {
    if (jobs[i].status == JOB_DONE)
      continue;
    const char *st = (jobs[i].status == JOB_RUNNING) ? "Running" : "Stopped";
    printf("[%d] %s\t\t%s &\n", jobs[i].id, st, jobs[i].cmdline);
  }
}

/* ─────────────────────────── last exit status ──────────────────── */
static int last_status = 0;

/* ─────────────────────────── foreground pid ────────────────────── */
/*
 * We save the foreground child pid so SIGINT is forwarded to it
 * instead of killing the shell.
 */
static volatile pid_t fg_pid = 0;

static void sigint_handler(int sig) {
  (void)sig;
  if (fg_pid > 0)
    kill(fg_pid, SIGINT);
  else {
    /* No foreground child — just reprint the prompt */
    write(STDOUT_FILENO, "\n", 1);
  }
}

static void sigchld_handler(int sig) {
  (void)sig;
  /* Handled in main loop via waitpid WNOHANG */
}

/* ─────────────────────────── env var expansion ─────────────────── */
/*
 * Expand $VAR and $? in a token.
 * Returns a newly malloc'd string; caller must free.
 */
static char *expand_var(const char *token) {
  char out[MAX_CMD_LEN];
  int oi = 0;
  int ti = 0;
  int len = (int)strlen(token);

  while (ti < len && oi < (int)sizeof(out) - 1) {
    if (token[ti] == '$') {
      ti++;
      if (token[ti] == '?') {
        /* $? = last exit status */
        char num[16];
        snprintf(num, sizeof(num), "%d", last_status);
        int nl = (int)strlen(num);
        if (oi + nl < (int)sizeof(out) - 1) {
          memcpy(&out[oi], num, nl);
          oi += nl;
        }
        ti++;
      } else if (token[ti] == '{') {
        /* ${VAR} form */
        ti++;
        char varname[128];
        int vi = 0;
        while (token[ti] && token[ti] != '}' && vi < (int)sizeof(varname) - 1)
          varname[vi++] = token[ti++];
        varname[vi] = '\0';
        if (token[ti] == '}')
          ti++;
        const char *val = getenv(varname);
        if (val) {
          int vl = (int)strlen(val);
          if (oi + vl < (int)sizeof(out) - 1) {
            memcpy(&out[oi], val, vl);
            oi += vl;
          }
        }
      } else {
        /* $VAR form */
        char varname[128];
        int vi = 0;
        while (token[ti] &&
               (token[ti] == '_' || (token[ti] >= 'A' && token[ti] <= 'Z') ||
                (token[ti] >= 'a' && token[ti] <= 'z') ||
                (token[ti] >= '0' && token[ti] <= '9')) &&
               vi < (int)sizeof(varname) - 1)
          varname[vi++] = token[ti++];
        varname[vi] = '\0';
        if (vi > 0) {
          const char *val = getenv(varname);
          if (val) {
            int vl = (int)strlen(val);
            if (oi + vl < (int)sizeof(out) - 1) {
              memcpy(&out[oi], val, vl);
              oi += vl;
            }
          }
        } else {
          out[oi++] = '$';
        }
      }
    } else {
      out[oi++] = token[ti++];
    }
  }
  out[oi] = '\0';
  return strdup(out);
}

/* ─────────────────────────── command struct ────────────────────── */
typedef struct {
  char *argv[MAX_ARGS]; /* NULL-terminated argument list       */
  int argc;
  char *redir_in;     /* < file                              */
  char *redir_out;    /* > file                              */
  char *redir_append; /* >> file                             */
  int background;     /* & flag (only meaningful on last)    */
} Cmd;

/* ─────────────────────────── parser ────────────────────────────── */
/*
 * Parse a line into an array of Cmd structs (pipeline stages).
 * Returns number of stages, 0 on empty/error.
 * Caller is responsible for freeing argv strings if expand was done.
 */
static int parse_line(char *line, Cmd cmds[MAX_PIPELINE]) {
  int nstage = 0;
  int background = 0;

  /* Check for background operator at the very end */
  size_t llen = strlen(line);
  if (llen > 0 && line[llen - 1] == '&') {
    background = 1;
    line[llen - 1] = '\0';
    /* trim trailing spaces */
    while (llen > 1 && line[llen - 2] == ' ')
      line[--llen - 1] = '\0';
  }

  /* Split on | */
  char *stages[MAX_PIPELINE];
  int ns = 0;
  char *p = line;
  char *pipe_pos;
  while ((pipe_pos = strchr(p, '|')) != NULL && ns < MAX_PIPELINE - 1) {
    *pipe_pos = '\0';
    stages[ns++] = p;
    p = pipe_pos + 1;
  }
  stages[ns++] = p;

  for (int s = 0; s < ns; s++) {
    Cmd *cmd = &cmds[nstage];
    memset(cmd, 0, sizeof(*cmd));
    cmd->background = (s == ns - 1) ? background : 0;

    /* Tokenize this stage */
    char *tok = strtok(stages[s], " \t");
    while (tok != NULL && cmd->argc < MAX_ARGS - 1) {
      if (strcmp(tok, "<") == 0) {
        tok = strtok(NULL, " \t");
        if (tok)
          cmd->redir_in = tok;
      } else if (strcmp(tok, ">>") == 0) {
        tok = strtok(NULL, " \t");
        if (tok)
          cmd->redir_append = tok;
      } else if (strcmp(tok, ">") == 0) {
        tok = strtok(NULL, " \t");
        if (tok)
          cmd->redir_out = tok;
      } else {
        /* Expand env vars */
        cmd->argv[cmd->argc++] = expand_var(tok);
      }
      tok = strtok(NULL, " \t");
    }
    cmd->argv[cmd->argc] = NULL;

    if (cmd->argc > 0)
      nstage++;
  }

  return nstage;
}

static void free_cmd_args(Cmd cmds[], int n) {
  for (int i = 0; i < n; i++)
    for (int j = 0; j < cmds[i].argc; j++)
      free(cmds[i].argv[j]);
}

/* ─────────────────────────── built-ins ─────────────────────────── */
static int builtin_cd(Cmd *cmd) {
  const char *dir = cmd->argv[1];
  if (!dir)
    dir = getenv("HOME");
  if (!dir)
    dir = "/";

  struct stat st;
  if (stat(dir, &st) != 0) {
    fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
    return 1;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "cd: %s: Not a directory\n", dir);
    return 1;
  }
  if (access(dir, X_OK) != 0) {
    fprintf(stderr, "cd: %s: Permission denied (no execute bit)\n", dir);
    return 1;
  }
  if (chdir(dir) != 0) {
    fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
    return 1;
  }
  return 0;
}

static int builtin_echo(Cmd *cmd) {
  int newline = 1;
  int start = 1;
  if (cmd->argv[1] && strcmp(cmd->argv[1], "-n") == 0) {
    newline = 0;
    start = 2;
  }
  for (int i = start; cmd->argv[i]; i++) {
    if (i > start)
      printf(" ");
    printf("%s", cmd->argv[i]);
  }
  if (newline)
    printf("\n");
  return 0;
}

static int builtin_export(Cmd *cmd) {
  if (!cmd->argv[1]) {
    /* Print all env vars */
    extern char **environ;
    for (char **e = environ; *e; e++)
      printf("export %s\n", *e);
    return 0;
  }
  /* export KEY=VALUE or export KEY (with pre-set value) */
  char *eq = strchr(cmd->argv[1], '=');
  if (eq) {
    *eq = '\0';
    setenv(cmd->argv[1], eq + 1, 1);
  } else {
    /* Just mark existing var for export (no-op in our model) */
    const char *val = getenv(cmd->argv[1]);
    if (val)
      setenv(cmd->argv[1], val, 1);
  }
  return 0;
}

static int builtin_unset(Cmd *cmd) {
  if (cmd->argv[1])
    unsetenv(cmd->argv[1]);
  return 0;
}

static int builtin_env(void) {
  extern char **environ;
  for (char **e = environ; *e; e++)
    printf("%s\n", *e);
  return 0;
}

static int builtin_jobs(void) {
  jobs_print();
  return 0;
}

static int builtin_fg(Cmd *cmd) {
  int id = cmd->argv[1] ? atoi(cmd->argv[1]) : -1;
  Job *j = NULL;
  if (id > 0)
    j = job_by_id(id);
  else {
    /* Find most recent running job */
    for (int i = job_count - 1; i >= 0; i--) {
      if (jobs[i].status == JOB_RUNNING) {
        j = &jobs[i];
        break;
      }
    }
  }
  if (!j) {
    fprintf(stderr, "lssh: fg: no such job\n");
    return 1;
  }
  printf("%s\n", j->cmdline);
  fg_pid = j->pid;
  int status;
  waitpid(j->pid, &status, 0);
  fg_pid = 0;
  last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  j->status = JOB_DONE;
  j->exit_code = last_status;
  return last_status;
}

static int builtin_kill_cmd(Cmd *cmd) {
  int sig = SIGTERM;
  int start = 1;
  if (cmd->argv[1] && cmd->argv[1][0] == '-') {
    sig = atoi(&cmd->argv[1][1]);
    start = 2;
  }
  for (int i = start; cmd->argv[i]; i++) {
    pid_t pid = (pid_t)atoi(cmd->argv[i]);
    if (pid > 0)
      kill(pid, sig);
    else
      fprintf(stderr, "lssh: kill: invalid pid '%s'\n", cmd->argv[i]);
  }
  return 0;
}

static int builtin_help(void) {
  printf("lssh - LS-OpenServer Shell\n"
         "\n"
         "Built-in commands:\n"
         "  cd [dir]          Change directory (default: HOME or /)\n"
         "  /path  ./p  ../p  Implicit cd: bare path to a directory\n"
         "  pwd               Print working directory\n"
         "  echo [-n] [args]  Print arguments\n"
         "  export [KEY=VAL]  Set/show environment variables\n"
         "  unset KEY         Remove environment variable\n"
         "  env               Print all environment variables\n"
         "  history           Show command history\n"
         "  !n                Recall history entry n\n"
         "  !!                Recall last command\n"
         "  jobs              List background jobs\n"
         "  fg [n]            Bring job n to foreground\n"
         "  kill [-SIG] pid   Send signal to process\n"
         "  help              Show this message\n"
         "  exit [code]       Exit the shell\n"
         "\n"
         "Operators:\n"
         "  cmd1 | cmd2       Pipe stdout of cmd1 to stdin of cmd2\n"
         "  cmd > file        Redirect stdout to file (overwrite)\n"
         "  cmd >> file       Redirect stdout to file (append)\n"
         "  cmd < file        Redirect stdin from file\n"
         "  cmd &             Run command in background\n"
         "\n"
         "Variables:\n"
         "  $VAR  ${VAR}      Expand environment variable\n"
         "  $?                Last command exit status\n");
  return 0;
}

/* ─────────────────────────── execute pipeline ──────────────────── */
/*
 * Run an array of Cmd structs as a pipeline.
 * Handles single commands (no pipe) as a special case for clarity.
 */
static int run_pipeline(Cmd cmds[], int n) {
  if (n == 0)
    return 0;

  /* ── single command: check built-ins first ───────────────── */
  if (n == 1) {
    Cmd *c = &cmds[0];
    if (!c->argv[0])
      return 0;

    if (strcmp(c->argv[0], "exit") == 0) {
      int code = c->argv[1] ? atoi(c->argv[1]) : 0;
      free_cmd_args(cmds, n);
      exit(code);
    }
    if (strcmp(c->argv[0], "cd") == 0)
      return builtin_cd(c);
    if (strcmp(c->argv[0], "pwd") == 0) {
      char buf[MAXPATHLEN];
      if (getcwd(buf, sizeof(buf)))
        printf("%s\n", buf);
      return 0;
    }
    if (strcmp(c->argv[0], "echo") == 0)
      return builtin_echo(c);
    if (strcmp(c->argv[0], "export") == 0)
      return builtin_export(c);
    if (strcmp(c->argv[0], "unset") == 0)
      return builtin_unset(c);
    if (strcmp(c->argv[0], "env") == 0)
      return builtin_env();
    if (strcmp(c->argv[0], "history") == 0) {
      history_print();
      return 0;
    }
    if (strcmp(c->argv[0], "jobs") == 0)
      return builtin_jobs();
    if (strcmp(c->argv[0], "fg") == 0)
      return builtin_fg(c);
    if (strcmp(c->argv[0], "kill") == 0)
      return builtin_kill_cmd(c);
    if (strcmp(c->argv[0], "help") == 0)
      return builtin_help();

    /*
     * Implicit cd: if the sole argument looks like a path
     * (starts with /, ./, or ../) AND it is a directory,
     * treat it as "cd <path>" regardless of trailing slash.
     *
     * Examples:
     *   /System       -> cd /System
     *   /System/      -> cd /System
     *   ./foo         -> cd ./foo
     *   ../           -> cd ..
     */
    const char *arg = c->argv[0];
    if (c->argc == 1 && (arg[0] == '/' || (arg[0] == '.' && arg[1] == '/') ||
                         (arg[0] == '.' && arg[1] == '.' && arg[2] == '/'))) {
      struct stat st;
      /* Strip trailing slash for stat (except bare "/") */
      char stripped[MAXPATHLEN];
      strncpy(stripped, arg, sizeof(stripped) - 1);
      stripped[sizeof(stripped) - 1] = '\0';
      size_t slen = strlen(stripped);
      if (slen > 1 && stripped[slen - 1] == '/')
        stripped[slen - 1] = '\0';

      if (stat(stripped, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Delegate to builtin_cd for consistent error messages */
        Cmd fake = *c;
        fake.argv[0] = "cd";
        fake.argv[1] = stripped;
        fake.argv[2] = NULL;
        fake.argc = 2;
        return builtin_cd(&fake);
      }
      /* Not a directory — fall through to execvp which will give
       * a meaningful error (or run it if it really is executable) */
    }
  }

  /* ── build pipe chain ─────────────────────────────────────── */
  int pipes[MAX_PIPELINE - 1][2];
  for (int i = 0; i < n - 1; i++) {
    if (pipe(pipes[i]) == -1) {
      perror("pipe");
      return 1;
    }
  }

  pid_t pids[MAX_PIPELINE];
  int background = cmds[n - 1].background;

  /* Store cmdline for job table */
  char jcmd[256] = "";
  for (int i = 0; i < n; i++) {
    if (i > 0)
      strncat(jcmd, " | ", sizeof(jcmd) - strlen(jcmd) - 1);
    if (cmds[i].argv[0])
      strncat(jcmd, cmds[i].argv[0], sizeof(jcmd) - strlen(jcmd) - 1);
  }

  for (int s = 0; s < n; s++) {
    Cmd *c = &cmds[s];

    pid_t pid = fork();
    if (pid == 0) {
      /* ── child ───────────────────────────────────────── */

      /* Connect pipes */
      if (s > 0)
        dup2(pipes[s - 1][0], STDIN_FILENO);
      if (s < n - 1)
        dup2(pipes[s][1], STDOUT_FILENO);

      /* Close all pipe ends in child */
      for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
      }

      /* I/O redirection */
      if (c->redir_in) {
        int fd = open(c->redir_in, O_RDONLY);
        if (fd == -1) {
          perror(c->redir_in);
          _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
      }
      if (c->redir_out) {
        int fd = open(c->redir_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
          perror(c->redir_out);
          _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
      if (c->redir_append) {
        int fd = open(c->redir_append, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
          perror(c->redir_append);
          _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }

      /* Restore default SIGINT in child */
      signal(SIGINT, SIG_DFL);

      execvp(c->argv[0], c->argv);
      fprintf(stderr, "lssh: %s: %s\n", c->argv[0], strerror(errno));
      _exit(127);

    } else if (pid < 0) {
      perror("fork");
      pids[s] = -1;
    } else {
      pids[s] = pid;
    }
  }

  /* Close all pipe ends in parent */
  for (int i = 0; i < n - 1; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }

  if (background) {
    /* Don't wait; register in job table */
    Job *j = job_add(pids[n - 1], jcmd);
    if (j)
      printf("[%d] %d\n", j->id, pids[n - 1]);
    return 0;
  }

  /* Wait for all stages; capture exit code of last */
  int exit_code = 0;
  for (int s = 0; s < n; s++) {
    if (pids[s] < 0)
      continue;
    fg_pid = pids[s]; /* forward Ctrl-C to last foreground stage */
    int status;
    waitpid(pids[s], &status, 0);
    if (s == n - 1)
      exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
  fg_pid = 0;
  return exit_code;
}

/* ─────────────────────────── prompt ────────────────────────────── */
static void print_prompt(void) {
  char cwd[MAXPATHLEN];
  const char *home = getenv("HOME");

  if (!getcwd(cwd, sizeof(cwd)))
    strncpy(cwd, "?", sizeof(cwd));

  /* Replace $HOME prefix with ~ */
  char display[MAXPATHLEN];
  if (home && strncmp(cwd, home, strlen(home)) == 0)
    snprintf(display, sizeof(display), "~%s", cwd + strlen(home));
  else
    strncpy(display, cwd, sizeof(display));

  /* Colour: green user@host, blue path, white # */
  const char *user = getenv("USER");
  if (!user)
    user = "root";

  char hostname[64] = "lsopenserver";
  gethostname(hostname, sizeof(hostname));

  /* status indicator: red # if last command failed */
  const char *color_status = (last_status == 0) ? "\033[1;32m"  /* green */
                                                : "\033[1;31m"; /* red   */

  printf("\033[0m%s%s@%s\033[0m:\033[1;34m%s\033[0m%s# \033[0m", color_status,
         user, hostname, display, color_status);
  fflush(stdout);
}

/* ─────────────────────────── main ──────────────────────────────── */
int main(void) {
  /* Signal setup */
  signal(SIGINT, sigint_handler);
  signal(SIGCHLD, sigchld_handler);
  signal(SIGTTOU, SIG_IGN); /* suppress background tty output warnings */
  signal(SIGTTIN, SIG_IGN);

  printf("Welcome to LS-OpenServer Shell (lssh)\n");
  printf("Type 'help' for a list of built-in commands.\n");

  char line[MAX_CMD_LEN];

  while (1) {
    /* Reap any finished background jobs */
    jobs_reap();

    print_prompt();

    if (fgets(line, sizeof(line), stdin) == NULL) {
      if (feof(stdin)) {
        printf("\nexit\n");
        break;
      }
      /* EINTR from signal — just retry */
      clearerr(stdin);
      continue;
    }

    /* Strip trailing newline */
    line[strcspn(line, "\n")] = '\0';
    if (strlen(line) == 0)
      continue;

    /* History expansion (!n / !!) */
    const char *expanded = history_expand(line);
    if (!expanded)
      continue; /* bad history ref         */
    if (expanded != line) {
      printf("%s\n", expanded); /* echo the recalled cmd   */
      strncpy(line, expanded, sizeof(line) - 1);
    }

    history_add(line);

    /* Parse and execute */
    Cmd cmds[MAX_PIPELINE];
    int n = parse_line(line, cmds);
    if (n > 0) {
      last_status = run_pipeline(cmds, n);
      free_cmd_args(cmds, n);
    }
  }

  return last_status;
}