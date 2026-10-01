#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH_CAP 4096

static void
die(const char *msg)
{
        perror(msg);
        exit(1);
}

static int
write_text(const char *path, const char *text)
{
        int fd;
        size_t len;
        ssize_t n;

        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
                return -1;
        len = strlen(text);
        while (len != 0U) {
                n = write(fd, text, len);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        (void)close(fd);
                        return -1;
                }
                text += (size_t)n;
                len -= (size_t)n;
        }
        return close(fd);
}

static int
write_atomic(const char *dir, const char *name, const char *text)
{
        char tmp[PATH_CAP];
        char dst[PATH_CAP];

        if (snprintf(tmp, sizeof(tmp), "%s/.%s.tmp-%ld", dir, name,
            (long)getpid()) >= (int)sizeof(tmp) ||
            snprintf(dst, sizeof(dst), "%s/%s", dir, name) >=
            (int)sizeof(dst)) {
                errno = ENAMETOOLONG;
                return -1;
        }
        if (write_text(tmp, text) != 0)
                return -1;
        if (rename(tmp, dst) != 0) {
                (void)unlink(tmp);
                return -1;
        }
        return 0;
}

static int
mkdir_one(const char *path)
{
        if (mkdir(path, 0755) == 0)
                return 0;
        return errno == EEXIST ? 0 : -1;
}

static int
mkdirs(const char *path)
{
        char buf[PATH_CAP];
        char *p;

        if (strlen(path) >= sizeof(buf)) {
                errno = ENAMETOOLONG;
                return -1;
        }
        strcpy(buf, path);
        for (p = buf + 1; *p != '\0'; ++p) {
                if (*p != '/')
                        continue;
                *p = '\0';
                if (mkdir_one(buf) != 0)
                        return -1;
                *p = '/';
        }
        return mkdir_one(buf);
}

static int
read_line(const char *path, char *buf, size_t cap)
{
        FILE *fp;
        size_t n;

        fp = fopen(path, "r");
        if (fp == NULL)
                return -1;
        if (fgets(buf, (int)cap, fp) == NULL) {
                (void)fclose(fp);
                return -1;
        }
        (void)fclose(fp);
        n = strlen(buf);
        while (n != 0U && (buf[n - 1U] == '\n' || buf[n - 1U] == '\r'))
                buf[--n] = '\0';
        return 0;
}

static int
pid_alive(long pid)
{
        if (pid <= 0)
                return 0;
        if (kill((pid_t)pid, 0) == 0)
                return 1;
        return errno == EPERM;
}

static void
format_time(char *buf, size_t cap, time_t now)
{
        struct tm tmv;

        if (localtime_r(&now, &tmv) == NULL ||
            strftime(buf, cap, "%Y-%m-%dT%H:%M:%S%z", &tmv) == 0U)
                strcpy(buf, "UNKNOWN");
}

static int
write_command(const char *dir, char **argv)
{
        char path[PATH_CAP];
        FILE *fp;
        int i;

        if (snprintf(path, sizeof(path), "%s/command.txt", dir) >=
            (int)sizeof(path))
                return -1;
        fp = fopen(path, "w");
        if (fp == NULL)
                return -1;
        for (i = 0; argv[i] != NULL; ++i)
                fprintf(fp, "%s%s", i == 0 ? "" : " ", argv[i]);
        fputc('\n', fp);
        return fclose(fp);
}

static int
make_run_dir(const char *root, const char *name, char *out, size_t cap)
{
        char stamp[32];
        struct tm tmv;
        time_t now;
        int v;

        now = time(NULL);
        if (localtime_r(&now, &tmv) == NULL ||
            strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv) == 0U)
                return -1;
        if (mkdirs(root) != 0)
                return -1;
        for (v = 1; v < 10000; ++v) {
                if (snprintf(out, cap, "%s/%s-%s-v%d", root, name,
                    stamp, v) >= (int)cap) {
                        errno = ENAMETOOLONG;
                        return -1;
                }
                if (mkdir(out, 0755) == 0)
                        return 0;
                if (errno != EEXIST)
                        return -1;
        }
        errno = EEXIST;
        return -1;
}

static void
supervise(const char *dir, char **argv)
{
        char path[PATH_CAP];
        char buf[128];
        char when[64];
        int logfd;
        int status;
        pid_t child;
        int rc;

        if (setsid() < 0)
                _exit(125);
        if (snprintf(path, sizeof(path), "%s/harness.log", dir) >=
            (int)sizeof(path))
                _exit(125);
        logfd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd < 0)
                _exit(125);
        (void)close(STDIN_FILENO);
        if (open("/dev/null", O_RDONLY) != STDIN_FILENO)
                _exit(125);
        if (dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0)
                _exit(125);
        if (logfd > STDERR_FILENO)
                (void)close(logfd);

        snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
        (void)write_atomic(dir, "supervisor.pid", buf);
        format_time(when, sizeof(when), time(NULL));
        snprintf(buf, sizeof(buf), "%s\n", when);
        (void)write_atomic(dir, "started", buf);
        (void)write_atomic(dir, "state", "RUNNING\n");

        child = fork();
        if (child < 0) {
                (void)write_atomic(dir, "state", "SPAWN-FAILED\n");
                _exit(125);
        }
        if (child == 0) {
                if (setenv("P10JOB_RUN_DIR", dir, 1) != 0)
                        _exit(125);
                execvp(argv[0], argv);
                perror("execvp");
                _exit(127);
        }
        snprintf(buf, sizeof(buf), "%ld\n", (long)child);
        (void)write_atomic(dir, "child.pid", buf);

        do {
                rc = waitpid(child, &status, 0);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
                (void)write_atomic(dir, "state", "WAIT-FAILED\n");
                _exit(125);
        }
        if (WIFEXITED(status))
                rc = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
                rc = 128 + WTERMSIG(status);
        else
                rc = 125;
        snprintf(buf, sizeof(buf), "%d\n", rc);
        (void)write_atomic(dir, "exit-code", buf);
        format_time(when, sizeof(when), time(NULL));
        snprintf(buf, sizeof(buf), "%s\n", when);
        (void)write_atomic(dir, "finished", buf);
        (void)write_atomic(dir, "state", "EXITED\n");
        _exit(0);
}

static int
cmd_start(int argc, char **argv)
{
        const char *root;
        const char *name;
        char dir[PATH_CAP];
        pid_t pid;

        if (argc < 3) {
                fprintf(stderr, "usage: p10job start NAME -- COMMAND [ARGS...]\n");
                return 2;
        }
        name = argv[0];
        if (strcmp(argv[1], "--") != 0) {
                fprintf(stderr, "p10job: expected -- before command\n");
                return 2;
        }
        root = getenv("P10JOB_ROOT");
        if (root == NULL || *root == '\0')
                root = ".p10runs";
        if (strchr(name, '/') != NULL || strcmp(name, ".") == 0 ||
            strcmp(name, "..") == 0) {
                fprintf(stderr, "p10job: invalid job name\n");
                return 2;
        }
        if (make_run_dir(root, name, dir, sizeof(dir)) != 0)
                die("p10job: create run directory");
        if (write_command(dir, &argv[2]) != 0)
                die("p10job: write command");
        if (write_atomic(dir, "state", "STARTING\n") != 0)
                die("p10job: write initial state");
        pid = fork();
        if (pid < 0)
                die("p10job: fork");
        if (pid == 0)
                supervise(dir, &argv[2]);
        printf("%s\n", dir);
        return 0;
}

static int
cmd_status(const char *dir)
{
        char path[PATH_CAP];
        char state[64];
        char pidbuf[64];
        long pid;

        if (snprintf(path, sizeof(path), "%s/state", dir) >= (int)sizeof(path) ||
            read_line(path, state, sizeof(state)) != 0) {
                fprintf(stderr, "p10job: cannot read state for %s\n", dir);
                return 2;
        }
        printf("run_dir=%s\nstate=%s\n", dir, state);
        if (snprintf(path, sizeof(path), "%s/supervisor.pid", dir) <
            (int)sizeof(path) && read_line(path, pidbuf, sizeof(pidbuf)) == 0) {
                pid = strtol(pidbuf, NULL, 10);
                printf("supervisor_pid=%ld\nsupervisor_alive=%s\n", pid,
                    pid_alive(pid) ? "yes" : "no");
                if (strcmp(state, "RUNNING") == 0 && !pid_alive(pid))
                        printf("effective_state=LOST\n");
        }
        if (snprintf(path, sizeof(path), "%s/child.pid", dir) <
            (int)sizeof(path) && read_line(path, pidbuf, sizeof(pidbuf)) == 0) {
                pid = strtol(pidbuf, NULL, 10);
                printf("child_pid=%ld\nchild_alive=%s\n", pid,
                    pid_alive(pid) ? "yes" : "no");
        }
        if (snprintf(path, sizeof(path), "%s/exit-code", dir) <
            (int)sizeof(path) && read_line(path, pidbuf, sizeof(pidbuf)) == 0)
                printf("exit_code=%s\n", pidbuf);
        return 0;
}

static int
cmd_tail(const char *dir)
{
        char path[PATH_CAP];
        FILE *fp;
        char **lines;
        size_t cap;
        size_t count;
        size_t i;
        char buf[1024];

        if (snprintf(path, sizeof(path), "%s/harness.log", dir) >=
            (int)sizeof(path))
                return 2;
        fp = fopen(path, "r");
        if (fp == NULL)
                die("p10job: open harness.log");
        cap = 40U;
        count = 0U;
        lines = calloc(cap, sizeof(*lines));
        if (lines == NULL)
                die("p10job: calloc");
        while (fgets(buf, sizeof(buf), fp) != NULL) {
                char *copy = strdup(buf);
                if (copy == NULL)
                        die("p10job: strdup");
                if (count >= cap) {
                        free(lines[count % cap]);
                }
                lines[count % cap] = copy;
                ++count;
        }
        (void)fclose(fp);
        i = count > cap ? count - cap : 0U;
        for (; i < count; ++i)
                fputs(lines[i % cap], stdout);
        for (i = 0U; i < cap; ++i)
                free(lines[i]);
        free(lines);
        return 0;
}

int
main(int argc, char **argv)
{
        if (argc < 2) {
                fprintf(stderr, "usage: p10job start NAME -- COMMAND [ARGS...]\n"
                    "       p10job status RUN_DIR\n"
                    "       p10job tail RUN_DIR\n");
                return 2;
        }
        if (strcmp(argv[1], "start") == 0)
                return cmd_start(argc - 2, argv + 2);
        if (strcmp(argv[1], "status") == 0 && argc == 3)
                return cmd_status(argv[2]);
        if (strcmp(argv[1], "tail") == 0 && argc == 3)
                return cmd_tail(argv[2]);
        fprintf(stderr, "p10job: invalid command\n");
        return 2;
}
