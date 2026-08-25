#include "intrinsics.h"
#include "shell.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Defined in main.c.
 *
 * The directory in which the shell was started is the shell's
 * home directory for this assignment.
 */
extern char shell_home[PATH_MAX];

/*
 * Previous directory used by:
 *
 *     hop -
 */
static char previous_directory[PATH_MAX];
static int previous_directory_valid = 0;

/*
 * Change directory and update the previous-directory state
 * only after a successful chdir().
 */
static int change_directory(const char *path)
{
    char current_directory[PATH_MAX];

    if (getcwd(current_directory,
               sizeof(current_directory)) == NULL) {
        fprintf(stderr,
                "cshell: hop: getcwd: %s\n",
                strerror(errno));
        return -1;
    }

    if (chdir(path) < 0) {
        fprintf(stderr,
                "cshell: hop: %s: %s\n",
                path,
                strerror(errno));
        return -1;
    }

    /*
     * Store the directory from which we moved.
     */
    if (snprintf(previous_directory,
                 sizeof(previous_directory),
                 "%s",
                 current_directory) >=
        (int)sizeof(previous_directory)) {
        previous_directory_valid = 0;
    } else {
        previous_directory_valid = 1;
    }

    return 0;
}

static int execute_hop(int argc, char *const argv[])
{
    /*
     * "hop" with no arguments means hop to shell home.
     */
    if (argc == 1) {
        return change_directory(shell_home);
    }

    /*
     * Process multiple arguments from left to right.
     */
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        /*
         * "~" means shell home.
         */
        if (strcmp(path, "~") == 0) {
            path = shell_home;
        }

        /*
         * "-" means the previous directory.
         */
        else if (strcmp(path, "-") == 0) {
            if (!previous_directory_valid) {
                fprintf(stderr,
                        "cshell: hop: OLDPWD not set\n");
                return -1;
            }

            path = previous_directory;
        }

        if (change_directory(path) < 0) {
            return -1;
        }
    }

    return 0;
}

int is_intrinsic(const char *command)
{
    if (command == NULL) {
        return 0;
    }

    return strcmp(command, "hop") == 0;
}

int execute_intrinsic(int argc, char *const argv[])
{
    if (argc <= 0 ||
        argv == NULL ||
        argv[0] == NULL) {
        return -1;
    }

    if (strcmp(argv[0], "hop") == 0) {
        return execute_hop(argc, argv);
    }

    return -1;
}