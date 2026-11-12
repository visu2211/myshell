#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>

typedef struct { char **v; size_t n; char *in, *out; int append; } Command;
typedef struct { Command *v; size_t n; int exit_requested; } Job;
static void *resize(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { perror("realloc"); exit(EXIT_SUCCESS); }
    return q;
}
static char *copy(const char *s) {
    char *p = strdup(s);
    if (!p) { perror("strdup"); exit(EXIT_SUCCESS); }
    return p;
}
static int output(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        s += k; n -= (size_t)k;
    }
    return 0;
}
static void argument(Command *c, const char *s) {
    c->v = resize(c->v, (c->n + 2) * sizeof(*c->v));
    c->v[c->n++] = copy(s); c->v[c->n] = NULL;
}
static int compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
/* Only the final pathname component may contain the single wildcard. */
static void expand(Command *c, const char *s) {
    const char *star = strchr(s, '*'), *slash = strrchr(s, '/');
    if (!star || (slash && star < slash) || strchr(star + 1, '*')) {
        argument(c, s); return;
    }
    size_t prefix = slash ? (size_t)(slash - s + 1) : 0;
    char *directory = prefix ? strndup(s, prefix) : copy(".");
    if (!directory) { perror("strndup"); exit(EXIT_SUCCESS); }
    DIR *d = opendir(directory); free(directory);
    if (!d) { argument(c, s); return; }
    const char *base = s + prefix;
    size_t before = (size_t)(star - base), after = strlen(star + 1);
    size_t start = c->n;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        const char *name = entry->d_name; size_t len = strlen(name);
        if (name[0] == '.' && base[0] != '.') continue;
        if (len < before + after || strncmp(name, base, before) ||
            strcmp(name + len - after, star + 1)) continue;
        char *match = resize(NULL, prefix + len + 1);
        memcpy(match, s, prefix); strcpy(match + prefix, name);
        argument(c, match); free(match);
    }
    closedir(d);
    if (c->n == start) argument(c, s);
    else qsort(c->v + start, c->n - start, sizeof(*c->v), compare);
}
static void cleanup(Job *j) {
    for (size_t i = 0; i < j->n; ++i) {
        Command *c = &j->v[i];
        for (size_t k = 0; k < c->n; ++k) free(c->v[k]);
        free(c->v); free(c->in); free(c->out);
    }
    free(j->v);
}
static Command *add_command(Job *j) {
    j->v = resize(j->v, (j->n + 1) * sizeof(*j->v));
    Command *c = &j->v[j->n++]; memset(c, 0, sizeof(*c)); return c;
}
static int special(const char *s) {
    return !strcmp(s,"<") || !strcmp(s,">") || !strcmp(s,">>") || !strcmp(s,"|");
}
/* Operators are recognized even without the optional surrounding spaces. */
static int parse(char *line, Job *j) {
    char **tokens = NULL; size_t count = 0;
    for (char *p = line; *p && *p != '#';) {
        if (isspace((unsigned char)*p)) { ++p; continue; }
        char *start = p;
        if (*p == '>' && p[1] == '>') p += 2;
        else if (strchr("<>|", *p)) ++p;
        else while (*p && *p != '#' && !isspace((unsigned char)*p) && !strchr("<>|", *p)) ++p;
        char *token = strndup(start, (size_t)(p - start));
        if (!token) { perror("strndup"); exit(EXIT_SUCCESS); }
        tokens = resize(tokens, (count + 1) * sizeof(*tokens)); tokens[count++] = token;
    }
    int ok = 1;
    Command *c = count ? add_command(j) : NULL;
    for (size_t i = 0; i < count && ok; ++i) {
        char *t = tokens[i];
        if (!strcmp(t,"|")) {
            if (!c->n || i + 1 == count) { ok = 0; break; }
            c = add_command(j);
        } else if (special(t)) {
            if (!c->n || i + 1 == count || special(tokens[i+1])) { ok = 0; break; }
            char **target = !strcmp(t,"<") ? &c->in : &c->out;
            if (*target) { ok = 0; break; }
            *target = copy(tokens[++i]);
            if (target == &c->out) c->append = !strcmp(t,">>");
        } else {
            if (c->in || c->out) { ok = 0; break; }
            expand(c, t);
        }
    }
    for (size_t i = 0; i < count; ++i) free(tokens[i]);
    free(tokens);
    if (ok) for (size_t i = 0; i < j->n; ++i) {
        if (!j->v[i].n) ok = 0;
        else if (!strcmp(j->v[i].v[0],"exit")) j->exit_requested = 1;
    }
    if (!ok) output(2,"mysh: syntax error\n");
    return ok;
}
static char *resolve(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) == 0 ? copy(name) : NULL;
    const char *dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    for (size_t i = 0; i < 3; ++i) {
        size_t n = strlen(dirs[i]) + strlen(name) + 2;
        char *path = resize(NULL, n); snprintf(path, n, "%s/%s", dirs[i], name);
        if (!access(path, X_OK)) return path;
        free(path);
    }
    return NULL;
}
static int builtin_name(const char *s) {
    return !strcmp(s,"cd") || !strcmp(s,"pwd") || !strcmp(s,"which") || !strcmp(s,"exit");
}
static char *working_directory(void) {
    size_t n = 128;
    for (;;) {
        char *s = resize(NULL,n);
        if (getcwd(s,n)) return s;
        free(s); if (errno != ERANGE) return NULL;
        n *= 2;
    }
}
static int builtin(Command *c) {
    char **a = c->v;
    if (!strcmp(a[0],"exit")) return 0;
    if (!strcmp(a[0],"cd")) {
        if (c->n > 2) { output(2,"cd: too many arguments\n"); return 1; }
        const char *path = c->n == 2 ? a[1] : getenv("HOME");
        if (!path) { output(2,"cd: HOME is not set\n"); return 1; }
        if (chdir(path)) { perror("cd"); return 1; }
        return 0;
    }
    if (!strcmp(a[0],"pwd")) {
        if (c->n != 1) { output(2,"pwd: expected no arguments\n"); return 1; }
        char *s = working_directory();
        if (!s) { perror("pwd"); return 1; }
        int rc = output(1,s); if (!rc) rc = output(1,"\n"); free(s); return rc ? 1 : 0;
    }
    if (c->n < 2) { output(2,"which: expected at least one argument\n"); return 1; }
    int rc = 0;
    for (size_t i = 1; i < c->n; ++i) {
        char *path = resolve(a[i]);
        if (!path) { dprintf(2,"which: %s: not found\n",a[i]); rc = 1; }
        else { if (output(1,path) || output(1,"\n")) rc = 1; free(path); }
    }
    return rc;
}
static int redirect(Command *c) {
    if (c->in) {
        int fd = open(c->in,O_RDONLY);
        if (fd < 0) { perror(c->in); return -1; }
        int rc = dup2(fd,0); if (fd != 0) close(fd);
        if (rc < 0) { perror("dup2"); return -1; }
    }
    if (c->out) {
        int fd = open(c->out,O_WRONLY|O_CREAT|(c->append?O_APPEND:O_TRUNC),0640);
        if (fd < 0) { perror(c->out); return -1; }
        int rc = dup2(fd,1); if (fd != 1) close(fd);
        if (rc < 0) { perror("dup2"); return -1; }
    }
    return 0;
}
static int wait_for(pid_t pid) {
    int status;
    while (waitpid(pid,&status,0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid"); return 1 << 8;
    }
    return status;
}
/* Return a waitpid-style status for both built-ins and child jobs. */
static int execute(Job *j, int interactive, int input_fd) {
    if (j->n == 1 && builtin_name(j->v[0].v[0])) {
        int saved_in = dup(0), saved_out = dup(1);
        if (saved_in < 0 || saved_out < 0) {
            perror("dup"); if (saved_in >= 0) close(saved_in); if (saved_out >= 0) close(saved_out);
            return 1 << 8;
        }
        int rc = redirect(&j->v[0]) < 0 ? 1 : builtin(&j->v[0]);
        if (dup2(saved_in,0) < 0 || dup2(saved_out,1) < 0) perror("dup2");
        close(saved_in); close(saved_out); return rc << 8;
    }
    pid_t *pids = resize(NULL,j->n * sizeof(*pids));
    size_t started = 0; int previous = -1, failed = 0;
    for (size_t i = 0; i < j->n; ++i) {
        int ends[2] = {-1,-1};
        if (i + 1 < j->n && pipe(ends)) { perror("pipe"); failed = 1; break; }
        pid_t pid = fork();
        if (pid == 0) {
            if (input_fd > 2) close(input_fd);
            if (previous >= 0) {
                if (dup2(previous,0) < 0) { perror("dup2"); _exit(1); }
            } else if (!interactive) {
                int fd = open("/dev/null",O_RDONLY);
                if (fd < 0 || dup2(fd,0) < 0) { perror("/dev/null"); _exit(1); }
                if (fd != 0) close(fd);
            }
            if (ends[1] >= 0 && dup2(ends[1],1) < 0) { perror("dup2"); _exit(1); }
            if (previous >= 0) close(previous);
            if (ends[0] >= 0) close(ends[0]);
            if (ends[1] >= 0) close(ends[1]);
            Command *c = &j->v[i];
            if (redirect(c) < 0) _exit(1);
            if (builtin_name(c->v[0])) _exit(builtin(c));
            char *path = resolve(c->v[0]);
            if (!path) { dprintf(2,"mysh: %s: command not found or not executable\n",c->v[0]); _exit(127); }
            execv(path,c->v); perror(c->v[0]); free(path); _exit(126);
        }
        if (pid < 0) {
            perror("fork"); if (ends[0] >= 0) close(ends[0]); if (ends[1] >= 0) close(ends[1]);
            failed = 1; break;
        }
        pids[started++] = pid;
        if (previous >= 0) close(previous);
        if (ends[1] >= 0) close(ends[1]);
        previous = ends[0];
    }
    if (previous >= 0) close(previous);
    if (failed) for (size_t i = 0; i < started; ++i) kill(pids[i],SIGKILL);
    int status = 0;
    for (size_t i = 0; i < started; ++i) status = wait_for(pids[i]);
    free(pids); return failed ? 1 << 8 : status;
}
/* One-byte reads avoid consuming any subsequent command or terminal input. */
static int read_line(int fd, char **line) {
    size_t n = 0, cap = 128; *line = resize(NULL,cap);
    for (;;) {
        char ch; ssize_t got = read(fd,&ch,1);
        if (got < 0) { if (errno == EINTR) continue; perror("read"); free(*line); *line=NULL; return -1; }
        if (!got || ch == '\n') {
            (*line)[n] = '\0'; return got || n ? 1 : 0;
        }
        if (n + 1 >= cap) { cap *= 2; *line = resize(*line,cap); }
        (*line)[n++] = ch;
    }
}
static void prompt(void) {
    char *cwd = working_directory();
    if (!cwd) { perror("getcwd"); output(1,"$ "); return; }
    const char *home = getenv("HOME"); size_t n = home ? strlen(home) : 0;
    while (n > 1 && home[n-1] == '/') --n;
    if (n && !strncmp(cwd,home,n) && (cwd[n] == '/' || cwd[n] == '\0' || n == 1)) {
        output(1,"~");
        if (n == 1 && cwd[1]) output(1,"/");
        output(1,cwd+n);
    } else output(1,cwd);
    output(1,"$ "); free(cwd);
}
int main(int argc, char **argv) {
    if (argc > 2) { output(2,"usage: mysh [command-file]\n"); return EXIT_SUCCESS; }
    int fd = 0;
    if (argc == 2) {
        fd = open(argv[1],O_RDONLY);
        if (fd < 0) { perror(argv[1]); return EXIT_FAILURE; }
    }
    int interactive = argc == 1 && isatty(0);
    if (interactive) output(1,"Welcome to my shell!\n");
    for (;;) {
        if (interactive) prompt();
        char *line; int result = read_line(fd,&line);
        if (result <= 0) { free(line); break; }
        Job j = {0}; int valid = parse(line,&j); free(line);
        int status = valid && j.n ? execute(&j,interactive,fd) : (valid ? 0 : 1 << 8);
        if (interactive && (!valid || j.n)) {
            if (WIFSIGNALED(status)) dprintf(1,"Terminated by signal %s\n",strsignal(WTERMSIG(status)));
            else if (WIFEXITED(status) && WEXITSTATUS(status)) dprintf(1,"Exited with status %d\n",WEXITSTATUS(status));
        }
        int stop = valid && j.exit_requested; cleanup(&j);
        if (stop) break;
    }
    if (fd != 0) close(fd);
    if (interactive) output(1,"Exiting my shell.\n");
    return EXIT_SUCCESS;
}
