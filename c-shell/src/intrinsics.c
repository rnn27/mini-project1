#include "intrinsics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char shell_home[PATH_MAX];

static char previous_directory[PATH_MAX] = "";
static int previous_directory_valid = 0;

/*
 * Safely construct:
 *
 *     directory / name
 *
 * without triggering -Wformat-truncation.
 */
static int join_path(char *result,
                     size_t result_size,
                     const char *directory,
                     const char *name){
    if(result == NULL ||
        result_size == 0 ||
        directory == NULL ||
        name == NULL){
        return -1;
    }

    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);

    int need_separator =
        directory_length > 0 &&
        directory[directory_length - 1] != '/';

    size_t required =
        directory_length +
       (size_t)need_separator +
        name_length +
        1;

    if(required > result_size){
        return -1;
    }

    memcpy(result,
           directory,
           directory_length);

    size_t offset = directory_length;

    if(need_separator){
        result[offset++] = '/';
    }

    memcpy(result + offset,
           name,
           name_length);

    result[offset + name_length] = '\0';

    return 0;
}

/* ------------------------------------------------------------------------- */
/* B1: hop                                                                  */
/* ------------------------------------------------------------------------- */

static int change_directory(const char *path){
    char current_directory[PATH_MAX];

    if(getcwd(current_directory,
               sizeof(current_directory)) == NULL){
        fprintf(stderr,
                "hop: no such directory\n");
        return -1;
    }

    if(chdir(path) < 0){
        /*
         * Frecency fallback.
         */
        char history_file[PATH_MAX];

        if(join_path(history_file,
                      sizeof(history_file),
                      shell_home,
                      ".hop_history") < 0){
            fprintf(stderr,
                    "hop: no such directory\n");
            return -1;
        }

        FILE *file = fopen(history_file, "r");

        if(file != NULL){
            char line[PATH_MAX];

            while(fgets(line,
                         sizeof(line),
                         file) != NULL){

                line[strcspn(line, "\n")] = '\0';

                if(strstr(line, path) != NULL &&
                    access(line, F_OK) == 0){

                    if(chdir(line) == 0){
                        fclose(file);
                        goto success;
                    }
                }
            }

            fclose(file);
        }

        fprintf(stderr,
                "hop: no such directory\n");
        return -1;
    }

success:
    if(snprintf(previous_directory,
                 sizeof(previous_directory),
                 "%s",
                 current_directory) >=
       (int)sizeof(previous_directory)){

        previous_directory_valid = 0;
    } else{
        previous_directory_valid = 1;
    }

    /*
     * Record successful destination for frecency.
     */
    char new_directory[PATH_MAX];

    if(getcwd(new_directory,
               sizeof(new_directory)) != NULL){

        char history_file[PATH_MAX];

        if(join_path(history_file,
                      sizeof(history_file),
                      shell_home,
                      ".hop_history") == 0){

            FILE *file =
                fopen(history_file, "a");

            if(file != NULL){
                fprintf(file,
                        "%s\n",
                        new_directory);

                fclose(file);
            }
        }
    }

    return 0;
}

static int execute_hop(int argc,
                       char *const argv[]){
    if(argc == 1){
        return change_directory(shell_home);
    }

    for(int i = 1; i < argc; i++){
        const char *path = argv[i];

        if(strcmp(path, "~") == 0){
            path = shell_home;
        } else if(strcmp(path, "-") == 0){
            if(!previous_directory_valid){
                fprintf(stderr,
                        "hop: OLDPWD not set\n");
                return -1;
            }

            path = previous_directory;
        }

        if(change_directory(path) < 0){
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* B2: reveal                                                               */
/* ------------------------------------------------------------------------- */

static int cmp_entries(const void *a,
                       const void *b){
    const char *const *left =
       (const char *const *)a;

    const char *const *right =
       (const char *const *)b;

    return strcmp(*left, *right);
}

static int execute_reveal(int argc,
                          char *const argv[]){
    int show_all = 0;
    int recursive = 0;
    int arg_idx = 1;

    /*
     * Parse flags. Repeated combinations such as:
     *
     *     -ta
     *     -rrrrnnnn
     *
     * are accepted.
     */
    while(arg_idx < argc &&
           argv[arg_idx][0] == '-'){

        /*
         * A lone "-" is a directory target, not a flag.
         */
        if(strcmp(argv[arg_idx], "-") == 0){
            break;
        }

        for(size_t i = 1;
             argv[arg_idx][i] != '\0';
             i++){

            if(argv[arg_idx][i] == 'a'){
                show_all = 1;
            } else if(argv[arg_idx][i] == 't'){
                recursive = 1;
            } else{
                fprintf(stderr,
                        "reveal: invalid syntax\n");
                return -1;
            }
        }

        arg_idx++;
    }

    /*
     * At most one target is allowed.
     */
    if(argc - arg_idx > 1){
        fprintf(stderr,
                "reveal: invalid syntax\n");
        return -1;
    }

    const char *target =
       (arg_idx < argc)
            ? argv[arg_idx]
            : ".";

    char target_path[PATH_MAX];

    if(strcmp(target, "~") == 0){
        if(snprintf(target_path,
                     sizeof(target_path),
                     "%s",
                     shell_home) >=
           (int)sizeof(target_path)){
            fprintf(stderr,
                    "reveal: no such directory\n");
            return -1;
        }
    } else if(strcmp(target, "-") == 0){
        if(!previous_directory_valid){
            fprintf(stderr,
                    "reveal: no such directory\n");
            return -1;
        }

        if(snprintf(target_path,
                     sizeof(target_path),
                     "%s",
                     previous_directory) >=
           (int)sizeof(target_path)){
            fprintf(stderr,
                    "reveal: no such directory\n");
            return -1;
        }
    } else{
        if(snprintf(target_path,
                     sizeof(target_path),
                     "%s",
                     target) >=
           (int)sizeof(target_path)){
            fprintf(stderr,
                    "reveal: no such directory\n");
            return -1;
        }
    }

    DIR *dir = opendir(target_path);

    if(dir == NULL){
        fprintf(stderr,
                "reveal: no such directory\n");
        return -1;
    }

    struct dirent *entry;

    char **entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    while((entry = readdir(dir)) != NULL){
        if(!show_all &&
            entry->d_name[0] == '.'){
            continue;
        }

        if(count >= capacity){
            size_t new_capacity =
               (capacity == 0)
                    ? 16
                    : capacity * 2;

            char **new_entries =
                realloc(entries,
                        new_capacity *
                            sizeof(char *));

            if(new_entries == NULL){
                closedir(dir);

                for(size_t i = 0;
                     i < count;
                     i++){
                    free(entries[i]);
                }

                free(entries);

                return -1;
            }

            entries = new_entries;
            capacity = new_capacity;
        }

        entries[count] =
            strdup(entry->d_name);

        if(entries[count] == NULL){
            closedir(dir);

            for(size_t i = 0;
                 i < count;
                 i++){
                free(entries[i]);
            }

            free(entries);

            return -1;
        }

        count++;
    }

    closedir(dir);

    qsort(entries,
          count,
          sizeof(char *),
          cmp_entries);

    for(size_t i = 0;
         i < count;
         i++){

        printf("%s\n",
               entries[i]);

        if(recursive){
            char subpath[PATH_MAX];

            if(join_path(subpath,
                          sizeof(subpath),
                          target_path,
                          entries[i]) == 0){

                struct stat statbuf;

                if(stat(subpath,
                         &statbuf) == 0 &&
                    S_ISDIR(statbuf.st_mode)){

                    char *sub_argv[4];

                    sub_argv[0] = "reveal";
                    sub_argv[1] = show_all
                                     ? "-a"
                                     : "";
                    sub_argv[2] = subpath;
                    sub_argv[3] = NULL;

                    /*
                     * When recursive mode is active, preserve
                     * the show-all flag.
                     */
                   (void)execute_reveal(
                        3,
                        sub_argv
                    );
                }
            }
        }

        free(entries[i]);
    }

    free(entries);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* B3: peek                                                                 */
/* ------------------------------------------------------------------------- */

static int execute_peek(int argc,
                        char *const argv[]){
    int n_flag = 0;
    int r_flag = 0;
    int arg_idx = 1;

    while(arg_idx < argc &&
           argv[arg_idx][0] == '-' &&
           strcmp(argv[arg_idx], "-") != 0){

        for(size_t i = 1;
             argv[arg_idx][i] != '\0';
             i++){

            if(argv[arg_idx][i] == 'n'){
                n_flag = 1;
            } else if(argv[arg_idx][i] == 'r'){
                r_flag = 1;
            } else{
                fprintf(stderr,
                        "peek: invalid syntax\n");
                return -1;
            }
        }

        arg_idx++;
    }

    /*
     * No filenames means stdin.
     */
    if(arg_idx == argc){
        char line[4096];
        size_t line_number = 1;

        while(fgets(line,
                     sizeof(line),
                     stdin) != NULL){

            if(n_flag){
                printf("%zu ",
                       line_number++);
            }

            fputs(line, stdout);
        }

        return 0;
    }

    for(int file_index = arg_idx;
         file_index < argc;
         file_index++){

        const char *filename =
            argv[file_index];

        if(strcmp(filename, "-") == 0){
            char line[4096];
            size_t line_number = 1;

            while(fgets(line,
                         sizeof(line),
                         stdin) != NULL){

                if(n_flag){
                    printf("%zu ",
                           line_number++);
                }

                fputs(line, stdout);
            }

            continue;
        }

        FILE *file =
            fopen(filename, "r");

        if(file == NULL){
            fprintf(stderr,
                    "peek: no such file or directory\n");
            continue;
        }

        struct stat st;

        if(stat(filename, &st) == 0 &&
            S_ISDIR(st.st_mode)){

            fclose(file);

            fprintf(stderr,
                    "peek: is a directory\n");
            continue;
        }

        /*
         * Store lines for reverse mode.
         */
        if(!r_flag){
            char line[4096];
            size_t line_number = 1;

            while(fgets(line,
                         sizeof(line),
                         file) != NULL){

                if(n_flag){
                    printf("%zu ",
                           line_number++);
                }

                fputs(line, stdout);
            }

            fclose(file);
            continue;
        }

        char **lines = NULL;
        size_t line_count = 0;
        size_t line_capacity = 0;

        char buffer[4096];

        while(fgets(buffer,
                     sizeof(buffer),
                     file) != NULL){

            if(line_count >= line_capacity){
                size_t new_capacity =
                   (line_capacity == 0)
                        ? 32
                        : line_capacity * 2;

                char **new_lines =
                    realloc(lines,
                            new_capacity *
                                sizeof(char *));

                if(new_lines == NULL){
                    for(size_t i = 0;
                         i < line_count;
                         i++){
                        free(lines[i]);
                    }

                    free(lines);
                    fclose(file);

                    return -1;
                }

                lines = new_lines;
                line_capacity = new_capacity;
            }

            lines[line_count] =
                strdup(buffer);

            if(lines[line_count] == NULL){
                for(size_t i = 0;
                     i < line_count;
                     i++){
                    free(lines[i]);
                }

                free(lines);
                fclose(file);

                return -1;
            }

            line_count++;
        }

        fclose(file);

        /*
         * Reverse line order.
         *
         * Numbering corresponds to the original line numbers.
         */
        for(size_t i = line_count;
             i > 0;
             i--){

            size_t index = i - 1;

            if(n_flag){
                printf("%zu ",
                       index + 1);
            }

            fputs(lines[index],
                  stdout);
        }

        for(size_t i = 0;
             i < line_count;
             i++){
            free(lines[i]);
        }

        free(lines);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* B4: locate                                                                */
/* ------------------------------------------------------------------------- */

static int execute_locate(int argc,
                          char *const argv[]){
    if(argc < 2){
        fprintf(stderr,
                "locate: invalid syntax\n");
        return -1;
    }

    const char *path_env =
        getenv("PATH");

    if(path_env == NULL){
        path_env = "";
    }

    for(int i = 1;
         i < argc;
         i++){

        const char *name =
            argv[i];

        int found = 0;

        /*
         * First check current directory.
         */
        char local_path[PATH_MAX];

        if(join_path(local_path,
                      sizeof(local_path),
                      ".",
                      name) == 0 &&
            access(local_path, X_OK) == 0){

            char absolute_path[PATH_MAX];

            if(realpath(local_path,
                         absolute_path) != NULL){

                printf("%s\n",
                       absolute_path);
            } else{
                printf("%s/%s\n",
                       getenv("PWD") != NULL
                           ? getenv("PWD")
                           : ".",
                       name);
            }

            found = 1;
        }

        /*
         * Search every PATH entry in order.
         */
        char *path_copy =
            strdup(path_env);

        if(path_copy == NULL){
            return -1;
        }

        char *saveptr = NULL;

        char *directory =
            strtok_r(path_copy,
                     ":",
                     &saveptr);

        while(directory != NULL){
            char candidate[PATH_MAX];

            const char *dir =
                directory[0] == '\0'
                    ? "."
                    : directory;

            if(join_path(candidate,
                          sizeof(candidate),
                          dir,
                          name) == 0 &&
                access(candidate, X_OK) == 0){

                char absolute_path[PATH_MAX];

                if(realpath(candidate,
                             absolute_path) != NULL){

                    printf("%s\n",
                           absolute_path);
                    found = 1;
                }
            }

            directory =
                strtok_r(NULL,
                          ":",
                          &saveptr);
        }

        free(path_copy);

        if(!found){
            fprintf(stderr,
                    "locate: command not found(%s)\n",
                    name);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Intrinsic dispatch                                                        */
/* ------------------------------------------------------------------------- */

int is_intrinsic(const char *command){
    if(command == NULL){
        return 0;
    }

    return strcmp(command, "hop") == 0 ||
           strcmp(command, "reveal") == 0 ||
           strcmp(command, "peek") == 0 ||
           strcmp(command, "locate") == 0;
}

int execute_intrinsic(int argc,
                      char *const argv[]){
    if(argc <= 0 ||
        argv == NULL ||
        argv[0] == NULL){
        return -1;
    }

    if(strcmp(argv[0], "hop") == 0){
        return execute_hop(argc, argv);
    }

    if(strcmp(argv[0], "reveal") == 0){
        return execute_reveal(argc, argv);
    }

    if(strcmp(argv[0], "peek") == 0){
        return execute_peek(argc, argv);
    }

    if(strcmp(argv[0], "locate") == 0){
        return execute_locate(argc, argv);
    }

    return -1;
}