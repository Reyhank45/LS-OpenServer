/*
 * LS-OpenServer Init System
 * FreeBSD 14 PID 1 - init, service manager, daemon supervisor, system monitor
 *
 * Build:
 *   cc -O2 -o init init.c -lkvm
 *
 * Features:
 *   - Session/console setup
 *   - Service table with dependency ordering
 *   - Daemon supervisor with restart policy
 *   - System monitor via sysctl (CPU, RAM, uptime)
 *   - Signal handling (SIGCHLD, SIGTERM, SIGUSR1 for status dump)
 *   - Structured logging to /var/log/init.log
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vm/vm_param.h> /* VM_TOTAL */

/* ─────────────────────────── tunables ─────────────────────────── */

#define LOG_PATH "/System/var/log/init.log"
#define DEV_PATH "/System/dev"
#define CONSOLE_DEV "/System/dev/console"
#define NULL_DEV "/System/dev/null"
#define MAX_SERVICES 32
#define MAX_ARGS 16
#define MAX_ENV 16
#define MONITOR_INTERVAL 30 /* seconds between sysmon prints     */

/* ─────────────────────────── restart policy ───────────────────── */
typedef enum {
  RESTART_NEVER,  /* run once, never restart                     */
  RESTART_ALWAYS, /* restart unconditionally (daemon)            */
  RESTART_ONFAIL, /* restart only on non-zero exit               */
} RestartPolicy;

/* ─────────────────────────── service state ────────────────────── */
typedef enum {
  SVC_PENDING,
  SVC_RUNNING,
  SVC_EXITED,
  SVC_FAILED,
} SvcState;

/* ─────────────────────────── service descriptor ───────────────── */
typedef struct {
  const char *name;
  const char *argv[MAX_ARGS];
  const char *envp[MAX_ENV];
  RestartPolicy restart;
  int restart_delay; /* base seconds before restart   */
  int max_restarts;  /* 0 = unlimited                 */
  int priority;      /* lower = start first           */
  /* runtime */
  pid_t pid;
  SvcState state;
  int restart_count;
  int consecutive_fails; /* reset on clean exit (code 0)  */
  time_t last_start;
} Service;

/* ─────────────────────────── service table ────────────────────── */
/*
 * Add your services here.  argv[0] must be the full path to the
 * binary; the list must end with a NULL sentinel in argv[0].
 *
 * priority: services are started in ascending priority order.
 * Lower number = starts earlier (like rc(8) ordering).
 */
static Service services[] = {
    /* ── installer (runs once at first boot) ─────────────────── */
    {
        .name = "installer",
        .argv = {"/System/sbin/installer", NULL},
        .envp = {"PATH=/System/bin:/System/sbin", "HOME=/", "TERM=vt100", NULL},
        .restart = RESTART_NEVER,
        .restart_delay = 0,
        .priority = 0,
    },
    /* ── syslogd ──────────────────────────────────────────────── */
    {
        .name = "syslogd",
        .argv = {"/System/sbin/syslogd", "-s", NULL},
        .envp = {"PATH=/System/bin:/System/sbin", NULL},
        .restart = RESTART_ALWAYS,
        .restart_delay = 2,
        .max_restarts = 5, /* give up after 5 consecutive fails  */
        .priority = 10,
    },
    /* ── network ──────────────────────────────────────────────── */
    {
        .name = "netinit",
        .argv = {"/System/bin/netinit", NULL},
        .envp = {"PATH=/System/bin:/System/sbin", NULL},
        .restart = RESTART_NEVER,
        .restart_delay = 0,
        .priority = 20,
    },
    /* ── interactive shell ────────────────────────────────────── */
    {
        .name = "lssh",
        .argv = {"/System/bin/lssh", NULL},
        .envp = {"PATH=/System/bin:/System/sbin", "HOME=/", "TERM=vt100", NULL},
        .restart = RESTART_ALWAYS,
        .restart_delay = 1,
        .max_restarts = 0, /* shell: always restart             */
        .priority = 99,
    },
    /* sentinel */
            { .name = "httpd",
          .argv = {"/Server/httpserver/bin/httpd", NULL},
          .envp = {"PATH=/System/bin:/System/sbin", NULL},
          .restart = RESTART_ALWAYS,
          .restart_delay = 2,
          .priority = 30,
        },
        {.name = NULL}};

/* ─────────────────────────── globals ──────────────────────────── */
static FILE *logfp = NULL;
static int boot_done = 0; /* set after services start; stops
                             console spam during runtime    */
static volatile sig_atomic_t got_sigchld = 0;
static volatile sig_atomic_t got_sigterm = 0;
static volatile sig_atomic_t got_sigusr1 = 0; /* status dump     */
static time_t boot_time;

/* ─────────────────────────── logging ──────────────────────────── */
static void log_init(void) {
  logfp = fopen(LOG_PATH, "a");
  /* non-fatal: if we can't open the log, stderr still works */
}

static void logmsg(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logmsg(const char *level, const char *fmt, ...) {
  char buf[512];
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  /*
   * During boot (boot_done == 0) write to /dev/console so the
   * operator can see startup progress.
   * After boot_done is set, write to the log file ONLY — the shell
   * owns /dev/console by then and init messages would corrupt the
   * prompt.
   */
  if (!boot_done)
    fprintf(stderr, "[init] [%s] %s\n", level, buf);

  if (logfp) {
    fprintf(logfp, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
            tm->tm_min, tm->tm_sec, level, buf);
    fflush(logfp);
  }
}

#define LOG_INFO(...) logmsg("INFO ", __VA_ARGS__)
#define LOG_WARN(...) logmsg("WARN ", __VA_ARGS__)
#define LOG_ERR(...) logmsg("ERROR", __VA_ARGS__)

/* ─────────────────────────── signal handlers ──────────────────── */
static void handle_sigchld(int sig) {
  (void)sig;
  got_sigchld = 1;
}
static void handle_sigterm(int sig) {
  (void)sig;
  got_sigterm = 1;
}
static void handle_sigusr1(int sig) {
  (void)sig;
  got_sigusr1 = 1;
}

static void setup_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  sa.sa_handler = handle_sigchld;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);

  sa.sa_handler = handle_sigterm;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGTERM, &sa, NULL);

  sa.sa_handler = handle_sigusr1;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &sa, NULL);

  /* PID 1 must NOT ignore SIGINT or the keyboard will be deaf */
  sa.sa_handler = SIG_DFL;
  sigaction(SIGINT, &sa, NULL);
}

/* ─────────────────────────── console setup ────────────────────── */
static void mount_devfs(void) {
  /*
   * Mount devfs at /System/dev.  This must happen before any
   * call to open("/System/dev/console", ...) succeeds.
   * nmount(2) is the FreeBSD-native interface.
   */
  struct iovec iov[] = {
      {"fstype", sizeof("fstype")},
      {"devfs", sizeof("devfs")},
      {"fspath", sizeof("fspath")},
      {DEV_PATH, sizeof(DEV_PATH)},
  };
  if (nmount(iov, 4, 0) != 0 && errno != EBUSY) {
    /* Can't use logmsg yet — write direct to fd 2 (boot loader console) */
    const char *msg = "[init] WARNING: failed to mount devfs at " DEV_PATH "\n";
    write(STDERR_FILENO, msg, strlen(msg));
  }
}

static void setup_console(void) {
  int fd = open(CONSOLE_DEV, O_RDWR);
  if (fd == -1) {
    perror("open " CONSOLE_DEV);
    return;
  }
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);
  if (fd > 2)
    close(fd);
}

/* ─────────────────────────── service helpers ──────────────────── */

/* Count services (excluding sentinel) */
static int svc_count(void) {
  int n = 0;
  while (services[n].name != NULL)
    n++;
  return n;
}

/* Find service by pid */
static Service *svc_by_pid(pid_t pid) {
  int n = svc_count();
  for (int i = 0; i < n; i++)
    if (services[i].pid == pid)
      return &services[i];
  return NULL;
}

/* Spawn a service, sets svc->pid and svc->state */
static void svc_spawn(Service *svc) {
  pid_t pid = fork();
  if (pid == 0) {
    /* child */
    execve(svc->argv[0], (char *const *)svc->argv, (char *const *)svc->envp);
    /* if execve returns, something went wrong */
    fprintf(stderr, "[init] execve %s failed: %s\n", svc->argv[0],
            strerror(errno));
    _exit(1);
  } else if (pid > 0) {
    svc->pid = pid;
    svc->state = SVC_RUNNING;
    svc->last_start = time(NULL);
    svc->restart_count++;
    LOG_INFO("started service '%s' pid=%d (restart#%d)", svc->name, pid,
             svc->restart_count);
  } else {
    LOG_ERR("fork failed for '%s': %s", svc->name, strerror(errno));
    svc->state = SVC_FAILED;
  }
}

/* Start all services in priority order */
static void svc_start_all(void) {
  int n = svc_count();

  /* Bubble-sort by priority (n is small, fine) */
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (services[j].priority < services[i].priority) {
        Service tmp = services[i];
        services[i] = services[j];
        services[j] = tmp;
      }

  for (int i = 0; i < n; i++) {
    services[i].restart_count = 0;
    services[i].pid = -1;
    services[i].state = SVC_PENDING;

    /* RESTART_NEVER services with priority 0 run synchronously */
    if (services[i].restart == RESTART_NEVER && services[i].priority == 0) {
      svc_spawn(&services[i]);
      int status;
      waitpid(services[i].pid, &status, 0);
      services[i].state = SVC_EXITED;
      LOG_INFO("one-shot service '%s' finished (status=%d)", services[i].name,
               WEXITSTATUS(status));
      continue;
    }

    svc_spawn(&services[i]);
  }
}

/* ─────────────────────────── SIGCHLD reaper ───────────────────── */
static void reap_children(void) {
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    Service *svc = svc_by_pid(pid);
    if (svc == NULL) {
      LOG_WARN("reaped unknown pid=%d", pid);
      continue;
    }

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig_num = WIFSIGNALED(status) ? WTERMSIG(status) : -1;

    if (sig_num >= 0)
      LOG_WARN("service '%s' killed by signal %d", svc->name, sig_num);
    else
      LOG_INFO("service '%s' exited with code %d", svc->name, exit_code);

    /* Track consecutive failures */
    int failed = (exit_code != 0 && exit_code != -1) || sig_num >= 0;
    if (failed)
      svc->consecutive_fails++;
    else
      svc->consecutive_fails = 0;

    /*
     * Exit code 127 means execve itself failed (binary not found).
     * Retrying immediately is pointless — mark FAILED right away
     * unless max_restarts is 0 (unlimited), in which case we still
     * back off but keep trying (binary might appear after mount).
     */
    int enoent_exit = (exit_code == 127);

    /* Decide whether to restart */
    int should_restart = 0;
    switch (svc->restart) {
    case RESTART_ALWAYS:
      should_restart = 1;
      break;
    case RESTART_ONFAIL:
      should_restart = failed;
      break;
    case RESTART_NEVER:
      svc->state = SVC_EXITED;
      break;
    }

    /* Enforce max_restarts cap (0 = unlimited) */
    if (should_restart && svc->max_restarts > 0 &&
        svc->consecutive_fails >= svc->max_restarts) {
      LOG_ERR("service '%s' hit max restarts (%d consecutive fails) "
              "— marking FAILED",
              svc->name, svc->consecutive_fails);
      svc->state = SVC_FAILED;
      should_restart = 0;
    }

    /* Hard stop on missing binary after first attempt */
    if (should_restart && enoent_exit && svc->max_restarts > 0 &&
        svc->consecutive_fails >= 2) {
      LOG_ERR("service '%s': binary not found (%s) — giving up", svc->name,
              svc->argv[0]);
      svc->state = SVC_FAILED;
      should_restart = 0;
    }

    if (should_restart) {
      svc->state = SVC_EXITED;

      /*
       * Exponential backoff: delay doubles each consecutive fail,
       * capped at 60s.  Clean restarts use the base restart_delay.
       */
      int delay = svc->restart_delay;
      if (svc->consecutive_fails > 1) {
        delay = svc->restart_delay * (1 << (svc->consecutive_fails - 1));
        if (delay > 60)
          delay = 60;
      }

      LOG_INFO("will restart '%s' in %ds (fail#%d)", svc->name, delay,
               svc->consecutive_fails);
      if (delay > 0)
        sleep(delay);
      svc_spawn(svc);
    } else if (svc->state != SVC_FAILED) {
      svc->state = SVC_EXITED;
    }
  }
}

/* ─────────────────────────── system monitor ───────────────────── */

/*
 * Read a sysctl value into a buffer.
 * Returns 0 on success, -1 on error.
 */
static int sysctl_get(const char *name, void *buf, size_t *len) {
  return sysctlbyname(name, buf, len, NULL, 0);
}

static void print_sysmon(void) {
  /* ── uptime ──────────────────────────────────────────────── */
  time_t now = time(NULL);
  long uptime = (long)(now - boot_time);
  long days = uptime / 86400;
  long hours = (uptime % 86400) / 3600;
  long minutes = (uptime % 3600) / 60;

  /* ── memory ──────────────────────────────────────────────── */
  /*
   * FreeBSD exposes memory via vm.stats.vm.*
   * vm.stats.vm.v_page_count   = total pages
   * vm.stats.vm.v_free_count   = free pages
   * hw.pagesize                = page size in bytes
   */
  u_int pagesize = 0, total_pages = 0, free_pages = 0;
  size_t sz;

  sz = sizeof(pagesize);
  sysctl_get("hw.pagesize", &pagesize, &sz);

  sz = sizeof(total_pages);
  sysctl_get("vm.stats.vm.v_page_count", &total_pages, &sz);

  sz = sizeof(free_pages);
  sysctl_get("vm.stats.vm.v_free_count", &free_pages, &sz);

  unsigned long long total_mb =
      ((unsigned long long)total_pages * pagesize) / (1024 * 1024);
  unsigned long long free_mb =
      ((unsigned long long)free_pages * pagesize) / (1024 * 1024);
  unsigned long long used_mb = total_mb - free_mb;

  /* ── CPU load (1-minute load average) ───────────────────── */
  /*
   * FreeBSD vm.loadavg is a struct loadavg (from <sys/loadavg.h>).
   * Easier: use getloadavg(3).
   */
  double load[3] = {0, 0, 0};
  getloadavg(load, 3);

  /* ── service status ──────────────────────────────────────── */
  int n = svc_count();
  int running = 0, exited = 0, failed = 0;
  for (int i = 0; i < n; i++) {
    switch (services[i].state) {
    case SVC_RUNNING:
      running++;
      break;
    case SVC_EXITED:
      exited++;
      break;
    case SVC_FAILED:
      failed++;
      break;
    default:
      break;
    }
  }

  LOG_INFO("=== SYSMON ===");
  LOG_INFO("Uptime   : %ldd %ldh %ldm", days, hours, minutes);
  LOG_INFO("Memory   : %llu MB used / %llu MB total", used_mb, total_mb);
  LOG_INFO("Load avg : %.2f %.2f %.2f (1/5/15 min)", load[0], load[1], load[2]);
  LOG_INFO("Services : %d running, %d exited, %d failed (of %d)", running,
           exited, failed, n);
  LOG_INFO("==============");
}

/* ─────────────────────────── status dump (SIGUSR1) ────────────── */
static void dump_status(void) {
  int n = svc_count();
  LOG_INFO("=== SERVICE STATUS DUMP ===");
  for (int i = 0; i < n; i++) {
    const char *state_str;
    switch (services[i].state) {
    case SVC_PENDING:
      state_str = "PENDING";
      break;
    case SVC_RUNNING:
      state_str = "RUNNING";
      break;
    case SVC_EXITED:
      state_str = "EXITED";
      break;
    case SVC_FAILED:
      state_str = "FAILED";
      break;
    default:
      state_str = "UNKNOWN";
      break;
    }
    LOG_INFO("  [%d] %-12s  pid=%-6d  state=%-7s  restarts=%d",
             services[i].priority, services[i].name, services[i].pid, state_str,
             services[i].restart_count);
  }
  LOG_INFO("===========================");
  print_sysmon();
}

/* ─────────────────────────── graceful shutdown ─────────────────── */
static void do_shutdown(void) {
  LOG_INFO("SIGTERM received — shutting down services");
  int n = svc_count();
  for (int i = 0; i < n; i++) {
    if (services[i].state == SVC_RUNNING && services[i].pid > 0) {
      LOG_INFO("sending SIGTERM to '%s' pid=%d", services[i].name,
               services[i].pid);
      kill(services[i].pid, SIGTERM);
    }
  }
  /* Give services 5s to exit cleanly, then SIGKILL */
  sleep(5);
  for (int i = 0; i < n; i++) {
    if (services[i].state == SVC_RUNNING && services[i].pid > 0) {
      LOG_WARN("force-killing '%s' pid=%d", services[i].name, services[i].pid);
      kill(services[i].pid, SIGKILL);
    }
  }
  LOG_INFO("shutdown complete");
}

/* ─────────────────────────── main event loop ──────────────────── */
int main(void) {
  boot_time = time(NULL);

  /* PID 1 setup */
  setsid();
  mount_devfs(); /* must be first — populates /System/dev/ */
  setup_console();
  log_init();
  setup_signals();

  LOG_INFO("LS-OpenServer Init starting (FreeBSD 14, pid=1)");

  /* Start all services */
  svc_start_all();

  /*
   * Boot phase complete. From here on, /System/dev/console belongs
   * to the shell. Redirect init's own stderr to /System/dev/null so
   * any stray fprintf(stderr,...) calls don't corrupt the terminal.
   * All logging now goes to logfp only.
   */
  boot_done = 1;
  {
    int null_fd = open(NULL_DEV, O_WRONLY);
    if (null_fd != -1) {
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > 2)
        close(null_fd);
    }
  }
  LOG_INFO("boot complete — runtime logging to %s only", LOG_PATH);

  /* ── main loop ──────────────────────────────────────────── */
  time_t last_monitor = time(NULL);

  while (!got_sigterm) {
    /* Reap any dead children */
    if (got_sigchld) {
      got_sigchld = 0;
      reap_children();
    }

    /* Dump status on SIGUSR1  (kill -USR1 1) */
    if (got_sigusr1) {
      got_sigusr1 = 0;
      dump_status();
    }

    /* Periodic system monitor */
    time_t now = time(NULL);
    if (now - last_monitor >= MONITOR_INTERVAL) {
      last_monitor = now;
      print_sysmon();
    }

    /*
     * Sleep briefly so we're not busy-spinning.
     * On FreeBSD, pause() would be cleaner but won't wake for
     * our periodic monitor — sleep(1) is the simplest safe
     * approach for PID 1.
     */
    sleep(1);
  }

  do_shutdown();
  return 0;
}