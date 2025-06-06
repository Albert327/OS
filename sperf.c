
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <ctype.h>
#include <errno.h>


typedef struct {
    char *name;
    double total_time;
} SyscallEntry;

// 解析系统调用名称
char *extract_syscall_name(const char *line) {
    const char *p = line;
    while (*p && (isalnum(*p) || *p == '_') )p++;
    if (p == line) return NULL;
    return strndup(line, p - line);
}

// 解析耗时（秒）
double extract_time(const char *line) {
    const char *end = line + strlen(line);
    if (*(end - 1) == '\n') end--;
    if (end <= line || *(end - 1) != '>') return 0.0;

    const char *start_bracket = end - 1;
    while (start_bracket > line && *start_bracket != '<') start_bracket--;
    if (start_bracket == line || *start_bracket != '<') return 0.0;

    char *time_str = strndup(start_bracket + 1, end - start_bracket - 2);
    double time = atof(time_str);
    free(time_str);
    return time;
}

// 比较函数（用于排序）
int compare_entries(const void *a, const void *b) {
    const SyscallEntry *ea = (const SyscallEntry *)a;
    const SyscallEntry *eb = (const SyscallEntry *)b;
    return (ea->total_time < eb->total_time) ? 1 : -1;
}


void print_top5(SyscallEntry *entries, int count, double total_time) {
    qsort(entries, count, sizeof(SyscallEntry), compare_entries);

    int n = count > 5 ? 5 : count;
    for (int i = 0; i < n; i++) {
        int ratio = (int)(entries[i].total_time / total_time * 100 + 0.5);
        printf("%s (%d%%)\n", entries[i].name, ratio);
    }

    // 输出 80 个空字符
    char zeros[80] = {0};
    fwrite(zeros, 1, 80, stdout);
    fflush(stdout);
}

int main(int argc, char *argv[], char *envp[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s COMMAND [ARGS...]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 创建管道
    int pipefd[2];
    if (pipe(pipefd)) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {  // 子进程：执行 strace
        close(pipefd[0]);  // 关闭读端
        dup2(pipefd[1], STDERR_FILENO);  // 重定向 stderr
        close(pipefd[1]);

        // 构造 strace 参数
        char **new_argv = malloc((argc + 2) * sizeof(char *));
        new_argv[0] = "strace";
        new_argv[1] = "-T";
        for (int i = 1; i < argc; i++) {
            new_argv[i + 1] = argv[i];
        }
        new_argv[argc + 1] = NULL;

        // 尝试已知路径执行 strace
        char *paths[] = {"/bin/strace", "/usr/bin/strace", NULL};
        for (int i = 0; paths[i]; i++) {
            execve(paths[i], new_argv, envp);
        }
        perror("execve");
        exit(EXIT_FAILURE);
    } else {  // 父进程：处理 strace 输出
        close(pipefd[1]);  // 关闭写端
        FILE *fp = fdopen(pipefd[0], "r");
        if (!fp) {
            perror("fdopen");
            exit(EXIT_FAILURE);
        }

        SyscallEntry *entries = NULL;
        int count = 0, capacity = 0;
        double total_time = 0.0;
        struct timeval last_print, current_time;
        gettimeofday(&last_print, NULL);

        char *line = NULL;
        size_t len = 0;
        while (getline(&line, &len, fp) != -1) {
            // 解析系统调用名称和耗时
            char *name = extract_syscall_name(line);
            double time = extract_time(line);

            if (name && time > 0) {
                total_time += time;

                // 更新或添加条目
                int found = 0;
                for (int i = 0; i < count; i++) {
                    if (strcmp(entries[i].name, name) == 0) {
                        entries[i].total_time += time;
                        found = 1;
                        break;
                    }
                }

                if (!found) {
                    if (count >= capacity) {
                        capacity = capacity ? capacity * 2 : 16;
                        entries = realloc(entries, capacity * sizeof(SyscallEntry));
                    }
                    entries[count++] = (SyscallEntry){name, time};
                } else {
                    free(name);
                }


                gettimeofday(&current_time, NULL);
                double elapsed_ms = (current_time.tv_sec - last_print.tv_sec) * 1000.0 +
                                    (current_time.tv_usec - last_print.tv_usec) / 1000.0;

                if (elapsed_ms >= 100 && count > 0) {
                    print_top5(entries, count, total_time);
                    last_print = current_time;
                }
            }
            free(line);
            line = NULL;
        }


        if (count > 0) {
            print_top5(entries, count, total_time);
        }

        // 清理资源
        for (int i = 0; i < count; i++) free(entries[i].name);
        free(entries);
        fclose(fp);
        waitpid(pid, NULL, 0);  // 等待子进程结束
    }
    return 0;
}

