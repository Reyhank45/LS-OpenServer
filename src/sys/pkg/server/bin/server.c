#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#define PIDFILE "/System/var/run/server.pid"

void start_service() {
    if (access(PIDFILE, F_OK) == 0) {
        printf("service already running\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execlp("/Server/httpserver/bin/httpd", "httpd", NULL);
        _exit(1);
    } else if (pid > 0) {
        FILE *f = fopen(PIDFILE, "w");
        if (f) {
            fprintf(f, "%d", pid);
            fclose(f);
        }
        printf("service started with pid %d\n", pid);
    } else {
        perror("fork");
    }
}

void stop_service() {
    FILE *f = fopen(PIDFILE, "r");
    if (!f) { printf("service not running\n"); return; }
    int pid; fscanf(f, "%d", &pid); fclose(f);
    if (kill(pid, SIGTERM) == 0) {
        waitpid(pid, NULL, 0);
        printf("service stopped\n");
        remove(PIDFILE);
    } else {
        perror("kill");
    }
}

void status_service() {
    if (access(PIDFILE, F_OK) == 0) {
        printf("service is running\n");
    } else {
        printf("service is not running\n");
    }
}

void usage() {
    printf("server [start|stop|restart|status]\n");
}

int main(int argc, char *argv[]) {
    if (argc != 2) { usage(); return 1; }
    if (strcmp(argv[1], "start") == 0) {
        start_service();
    } else if (strcmp(argv[1], "stop") == 0) {
        stop_service();
    } else if (strcmp(argv[1], "restart") == 0) {
        stop_service();
        start_service();
    } else if (strcmp(argv[1], "status") == 0) {
        status_service();
    } else {
        usage();
        return 1;
    }
    return 0;
}
