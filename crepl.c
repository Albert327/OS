#include <stdio.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_DECLS 100
#define TEMPLATE "/tmp/crepl-XXXXXX"

typedef struct {
    char *proto;
} Declaration;

Declaration decls[MAX_DECLS]; 
int decl_count = 0;          
void *handles[100];           
int handle_count = 0;
static const int is_32bit = (sizeof(void*) == 4);       

void extract_prototype(const char *line, char **proto) {
    char *start = strchr(line, ' ') + 1;
    while (isspace(*start)) start++;
    
    char *name_end = strchr(start, '(');
    if (!name_end) return;
    size_t name_len = name_end - start;
    char func_name[256] = {0};
    strncpy(func_name, start, name_len > 255 ? 255 : name_len);

    char *param_start = name_end + 1;
    char *param_end = strchr(param_start, ')');
    if (!param_end) return;
    size_t params_len = param_end - param_start;
    char params[512] = {0};
    strncpy(params, param_start, params_len > 511 ? 511 : params_len);

    int param_count = 0;
    char *token = strtok(params, ",");
    while (token) {
        param_count++;
        token = strtok(NULL, ",");
    }

    char param_list[512] = {0};
    for (int i = 0; i < param_count; i++) {
        if (i > 0) strcat(param_list, ", ");
        strcat(param_list, "int");
    }

    int needed = snprintf(NULL, 0, "int %s(%s);", func_name, param_list);
    if (needed < 0) {
        *proto = NULL;
        return;
    }
    *proto = malloc(needed + 1);
    if (!*proto) {
        perror("malloc");
        return;
    }
    snprintf(*proto, needed + 1, "int %s(%s);", func_name, param_list);
}

int compile_so(const char *c_path, const char *so_path) {
    pid_t pid = fork();
    if (pid == 0) {
        if (is_32bit) {
            execlp("gcc", "gcc", "-m32", "-shared", "-fPIC", "-o", so_path, c_path,
                   "-Wl,--unresolved-symbols=ignore-all", NULL);
        } else {
            execlp("gcc", "gcc", "-shared", "-fPIC", "-o", so_path, c_path,
                   "-Wl,--unresolved-symbols=ignore-all", NULL);
        }
        _exit(EXIT_FAILURE);
    } else {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}

void process_expression(const char *line) {
    static int expr_id = 0;
    expr_id++;

    char c_path[] = TEMPLATE ".c";
    int fd = mkstemps(c_path, 2);
    if (fd == -1) {
        perror("mkstemps");
        return;
    }

    dprintf(fd, "// Auto-generated declarations\n");
    for (int i = 0; i < decl_count; i++) {
        dprintf(fd, "%s\n", decls[i].proto);
    }

    dprintf(fd, "int __expr_wrapper_%d() { return %s; }\n", expr_id, line);
    close(fd);

    char so_path[256];
    size_t len = strlen(c_path);
    if (len >= 2 && strcmp(c_path + len - 2, ".c") == 0) {
        snprintf(so_path, sizeof(so_path), "%.*s.so", (int)(len - 2), c_path);
    } else {
        snprintf(so_path, sizeof(so_path), "%s.so", c_path);
    }

    if (compile_so(c_path, so_path) != 0) {
        fprintf(stderr, "Compilation failed\n");
        unlink(c_path);
        return;
    }

    void *handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        unlink(c_path);
        unlink(so_path);
        return;
    }

    char func_name[64];
    snprintf(func_name, sizeof(func_name), "__expr_wrapper_%d", expr_id);
    int (*func)(void) = dlsym(handle, func_name);
    if (func) {
        printf("= %d\n", func());
    } else {
        fprintf(stderr, "dlsym error: %s\n", dlerror());
    }

    dlclose(handle);
    unlink(c_path);
    unlink(so_path);
}

void add_declaration(const char *proto) {
    if (decl_count >= MAX_DECLS) return;
    decls[decl_count++].proto = strdup(proto);
}

void cleanup() {
    for (int i = 0; i < decl_count; i++) {
        free(decls[i].proto);
    }
    for (int i = 0; i < handle_count; i++) {
        dlclose(handles[i]);
    }
}

int main() {
    char line[4096];
    atexit(cleanup);

    while (1) {
        printf("crepl> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "int ", 4) == 0) {
            char *proto = NULL;
            extract_prototype(line, &proto);
            if (!proto) {
                fprintf(stderr, "Error extracting prototype\n");
                continue;
            }

            char c_path[] = TEMPLATE ".c";
            int fd = mkstemps(c_path, 2);
            if (fd == -1) {
                perror("mkstemps");
                free(proto);
                continue;
            }
            write(fd, line, strlen(line));
            close(fd);

            char so_path[256];
            size_t len = strlen(c_path);
            if (len >= 2 && strcmp(c_path + len - 2, ".c") == 0) {
                snprintf(so_path, sizeof(so_path), "%.*s.so", (int)(len - 2), c_path);
            } else {
                snprintf(so_path, sizeof(so_path), "%s.so", c_path);
            }

            if (compile_so(c_path, so_path) == 0) {
                void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
                if (handle) {
                    handles[handle_count++] = handle;
                    add_declaration(proto);
                    printf("OK.\n");
                } else {
                    fprintf(stderr, "dlopen error: %s\n", dlerror());
                }
            }
            free(proto);
            unlink(c_path);
        } else {
            process_expression(line);
        }
    }

    return 0;
}
