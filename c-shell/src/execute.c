#include "execute.h"
#include "intrinsics.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_BACKGROUND 256

typedef struct{
    pid_t pid;
    pid_t pgid;
    int job_number;
    char command[PATH_MAX];
    int active;
    int stopped;
} BackgroundProcess;

static BackgroundProcess background_processes[MAX_BACKGROUND];
static int next_job_number=1;

static void write_number(char *buffer,size_t *length,pid_t value){
    char digits[32];
    size_t count=0;

    if(value==0){
        buffer[(*length)++]='0';
        return;
    }

    while(value>0){
        digits[count++]=(char)('0'+value%10);
        value/=10;
    }

    while(count>0){
        buffer[(*length)++]=digits[--count];
    }
}

static void background_message(const char *command,pid_t pid,int normal){
    char buffer[PATH_MAX+80];
    size_t length=0;
    const char *suffix=normal ? " exited normally\n" : " exited abnormally\n";

    while(command[length]!='\0' && length<PATH_MAX-1){
        buffer[length]=command[length];
        length++;
    }

    buffer[length++]=' ';
    buffer[length++]='w';
    buffer[length++]='i';
    buffer[length++]='t';
    buffer[length++]='h';
    buffer[length++]=' ';
    buffer[length++]='p';
    buffer[length++]='i';
    buffer[length++]='d';
    buffer[length++]=' ';

    write_number(buffer,&length,pid);

    size_t i=0;
    while(suffix[i]!='\0'){
        buffer[length++]=suffix[i++];
    }

    (void)write(STDOUT_FILENO,buffer,length);
}

static void sigchld_handler(int signal){
    (void)signal;

    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(!background_processes[i].active){
            continue;
        }

        int status;
        pid_t result=waitpid(background_processes[i].pid,&status,WNOHANG|WUNTRACED|WCONTINUED);

        if(result==background_processes[i].pid){
            if(WIFEXITED(status) || WIFSIGNALED(status)){
                background_message(background_processes[i].command,result,WIFEXITED(status));
                background_processes[i].active=0;
            }else if(WIFSTOPPED(status)){
                background_processes[i].stopped=1;
            }else if(WIFCONTINUED(status)){
                background_processes[i].stopped=0;
            }
        }
    }
}

static int add_background_process(pid_t pid,pid_t pgid,int job_number,const char *command){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(!background_processes[i].active){
            background_processes[i].pid=pid;
            background_processes[i].pgid=pgid;
            background_processes[i].job_number=job_number;
            strncpy(background_processes[i].command,command,PATH_MAX-1);
            background_processes[i].command[PATH_MAX-1]='\0';
            background_processes[i].active=1;
            background_processes[i].stopped=0;
            return 0;
        }
    }

    return -1;
}


int install_sigchld_handler(void){
    struct sigaction action;

    memset(&action,0,sizeof(action));
    action.sa_handler=sigchld_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags=SA_RESTART;

    return sigaction(SIGCHLD,&action,NULL);
}

static int block_sigchld(sigset_t *oldset){
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set,SIGCHLD);

    return sigprocmask(SIG_BLOCK,&set,oldset);
}

static void restore_sigchld(const sigset_t *oldset){
    sigprocmask(SIG_SETMASK,oldset,NULL);
}

static void cleanup_background_processes(void){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(!background_processes[i].active){
            continue;
        }

        int status;
        pid_t result;

        do{
            result=waitpid(background_processes[i].pid,
                            &status,
                            WNOHANG|WUNTRACED|WCONTINUED);

            if(result==background_processes[i].pid){
                if(WIFEXITED(status) || WIFSIGNALED(status)){
                    background_message(background_processes[i].command,
                                       result,
                                       WIFEXITED(status));
                    background_processes[i].active=0;
                }else if(WIFSTOPPED(status)){
                    background_processes[i].stopped=1;
                }else if(WIFCONTINUED(status)){
                    background_processes[i].stopped=0;
                }
            }
        }while(result==background_processes[i].pid &&
               background_processes[i].active);
    }
}

static int compare_activities(const void *a,const void *b){
    const BackgroundProcess *left=*(const BackgroundProcess *const *)a;
    const BackgroundProcess *right=*(const BackgroundProcess *const *)b;

    if(left->job_number<right->job_number){
        return -1;
    }

    if(left->job_number>right->job_number){
        return 1;
    }

    return 0;
}

int execute_activities(void){
    BackgroundProcess *groups[MAX_BACKGROUND];
    size_t group_count=0;

    sigset_t oldset;

    if(block_sigchld(&oldset)<0){
        return -1;
    }

    cleanup_background_processes();

    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(!background_processes[i].active){
            continue;
        }

        int found=0;

        for(size_t j=0;j<group_count;j++){
            if(groups[j]->job_number==background_processes[i].job_number){
                found=1;
                break;
            }
        }

        if(!found){
            groups[group_count++]=&background_processes[i];
        }
    }

    qsort(groups,group_count,sizeof(BackgroundProcess *),compare_activities);

    for(size_t i=0;i<group_count;i++){
        BackgroundProcess *group=groups[i];

        printf("[%d] pgid %d\n",
               group->job_number,
               (int)group->pgid);

        for(size_t j=0;j<MAX_BACKGROUND;j++){
            if(!background_processes[j].active ||
               background_processes[j].job_number!=group->job_number){
                continue;
            }

            printf("  %d %s %s\n",
                   (int)background_processes[j].pid,
                   background_processes[j].command,
                   background_processes[j].stopped ? "Stopped" : "Running");
        }
    }

    restore_sigchld(&oldset);

    return 0;
}

static int copy_fd(int source_fd,int destination_fd){
    char buffer[8192];

    while(1){
        ssize_t bytes_read=read(source_fd,buffer,sizeof(buffer));

        if(bytes_read==0){
            return 0;
        }

        if(bytes_read<0){
            if(errno==EINTR){
                continue;
            }

            return -1;
        }

        ssize_t total_written=0;

        while(total_written<bytes_read){
            ssize_t bytes_written=write(destination_fd,buffer+total_written,(size_t)(bytes_read-total_written));

            if(bytes_written<0){
                if(errno==EINTR){
                    continue;
                }

                return -1;
            }

            total_written+=bytes_written;
        }
    }
}

static int prepare_input_stream(const Command *command){
    if(command->input_redirection_count==0){
        return -1;
    }

    char template[]="/tmp/cshell-input-XXXXXX";
    int temp_fd=mkstemp(template);

    if(temp_fd<0){
        return -1;
    }

    if(unlink(template)<0){
        close(temp_fd);
        return -1;
    }

    for(size_t i=0;i<command->input_redirection_count;i++){
        const Redirection *redirection=&command->input_redirections[i];
        int input_fd=open(redirection->filename,O_RDONLY);

        if(input_fd<0){
            fprintf(stderr,"cshell: no such file or directory\n");
            close(temp_fd);
            return -1;
        }

        if(copy_fd(input_fd,temp_fd)<0){
            close(input_fd);
            close(temp_fd);
            return -1;
        }

        close(input_fd);
    }

    if(lseek(temp_fd,0,SEEK_SET)<0){
        close(temp_fd);
        return -1;
    }

    return temp_fd;
}

static int setup_input_for_child(const Command *command){
    if(command->input_redirection_count==0){
        return 0;
    }

    int input_fd=prepare_input_stream(command);

    if(input_fd<0){
        return -1;
    }

    if(dup2(input_fd,STDIN_FILENO)<0){
        close(input_fd);
        return -1;
    }

    close(input_fd);
    return 0;
}

static int open_output_files(const Command *command,int **fds_out){
    size_t count=command->output_redirection_count;
    int *fds=malloc(count*sizeof(int));

    if(fds==NULL){
        return -1;
    }

    for(size_t i=0;i<count;i++){
        const Redirection *redirection=&command->output_redirections[i];
        int flags=O_WRONLY|O_CREAT;

        if(redirection->type==REDIR_OUTPUT){
            flags|=O_TRUNC;
        }else{
            flags|=O_APPEND;
        }

        fds[i]=open(redirection->filename,flags,0644);

        if(fds[i]<0){
            for(size_t j=0;j<i;j++){
                close(fds[j]);
            }

            free(fds);
            fprintf(stderr,"cshell: unable to create file for writing\n");
            return -1;
        }
    }

    *fds_out=fds;
    return 0;
}

static int relay_output(int read_fd,const Command *command,int *output_fds){
    char buffer[8192];

    while(1){
        ssize_t bytes_read=read(read_fd,buffer,sizeof(buffer));

        if(bytes_read==0){
            return 0;
        }

        if(bytes_read<0){
            if(errno==EINTR){
                continue;
            }

            return -1;
        }

        for(size_t i=0;i<command->output_redirection_count;i++){
            ssize_t total_written=0;

            while(total_written<bytes_read){
                ssize_t bytes_written=write(output_fds[i],buffer+total_written,(size_t)(bytes_read-total_written));

                if(bytes_written<0){
                    if(errno==EINTR){
                        continue;
                    }

                    return -1;
                }

                total_written+=bytes_written;
            }
        }
    }
}

/* C1 command resolution */

static int is_regular_executable(const char *path){
    struct stat st;

    if(stat(path,&st)<0){
        return 0;
    }

    if(!S_ISREG(st.st_mode)){
        return 0;
    }

    return access(path,X_OK)==0;
}

static void execute_command(const Command *command){
    const char *original=command->argv[0];

    if(original==NULL || original[0]=='\0'){
        _exit(127);
    }

    if(is_intrinsic(original)){
        int result=execute_intrinsic((int)command->argc,command->argv);
        _exit(result==0 ? 0 : 1);
    }

    /* Search PATH directly for % commands. */
    if(original[0]=='%'){
        const char *name=original+1;

        if(name[0]=='\0'){
            fprintf(stderr,"cshell: command not found()\n");
            _exit(127);
        }

        char **exec_argv=malloc((command->argc+1)*sizeof(char *));

        if(exec_argv==NULL){
            perror("cshell: malloc");
            _exit(127);
        }

        for(size_t i=0;i<command->argc;i++){
            exec_argv[i]=command->argv[i];
        }

        exec_argv[command->argc]=NULL;
        exec_argv[0]=(char *)name;

        execvp(name,exec_argv);

        fprintf(stderr,"cshell: command not found(%s)\n",name);
        free(exec_argv);
        _exit(127);
    }

    /* Treat commands containing / as paths. */
    if(strchr(original,'/')!=NULL){
        if(access(original,X_OK)==0){
            execv(original,command->argv);
        }

        fprintf(stderr,"cshell: command not found(%s)\n",original);
        _exit(127);
    }

    /* Search the current directory first. */
    char local_path[PATH_MAX];
    int written=snprintf(local_path,sizeof(local_path),"./%s",original);

    if(written>=0 && (size_t)written<sizeof(local_path) && is_regular_executable(local_path)){
        execv(local_path,command->argv);
    }

    /* Search PATH after the current directory. */
    execvp(original,command->argv);

    fprintf(stderr,"cshell: command not found(%s)\n",original);
    _exit(127);
}

/* Wait for a child process. */

static int wait_for_pid(pid_t pid){
    int status;

    while(waitpid(pid,&status,0)<0){
        if(errno==EINTR){
            continue;
        }

        perror("cshell: waitpid");
        return -1;
    }

    return 0;
}

/* Execute one command. */

static int execute_simple_command(const Command *command,int background){
    int input_fd=-1;

    if(command->input_redirection_count>0){
        input_fd=prepare_input_stream(command);

        if(input_fd<0){
            return -1;
        }
    }

    int *output_fds=NULL;

    if(command->output_redirection_count>0){
        if(open_output_files(command,&output_fds)<0){
            if(input_fd>=0){
                close(input_fd);
            }

            return -1;
        }
    }

    int output_pipe[2]={-1,-1};

    if(command->output_redirection_count>0){
        if(pipe(output_pipe)<0){
            perror("cshell: pipe");

            for(size_t i=0;i<command->output_redirection_count;i++){
                close(output_fds[i]);
            }

            free(output_fds);

            if(input_fd>=0){
                close(input_fd);
            }

            return -1;
        }
    }

    sigset_t oldset;

    if(background && block_sigchld(&oldset)<0){
        perror("cshell: sigprocmask");

        if(output_pipe[0]>=0){
            close(output_pipe[0]);
            close(output_pipe[1]);
        }

        for(size_t i=0;i<command->output_redirection_count;i++){
            close(output_fds[i]);
        }

        free(output_fds);

        if(input_fd>=0){
            close(input_fd);
        }

        return -1;
    }

    pid_t pid=fork();

    if(pid<0){
        perror("cshell: fork");

        if(background){
            restore_sigchld(&oldset);
        }

        if(output_pipe[0]>=0){
            close(output_pipe[0]);
            close(output_pipe[1]);
        }

        for(size_t i=0;i<command->output_redirection_count;i++){
            close(output_fds[i]);
        }

        free(output_fds);

        if(input_fd>=0){
            close(input_fd);
        }

        return -1;
    }

    if(setpgid(pid,pid)<0 && errno!=EACCES && errno!=ESRCH){
        perror("cshell: setpgid");
    }

    if(pid==0){
        if(setpgid(0,0)<0){
            _exit(1);
        }

        if(background){
            sigset_t set;
            sigemptyset(&set);
            sigprocmask(SIG_SETMASK,&set,NULL);
        }

        if(input_fd>=0){
            if(dup2(input_fd,STDIN_FILENO)<0){
                _exit(1);
            }

            close(input_fd);
        }

        if(command->output_redirection_count>0){
            close(output_pipe[0]);

            if(background){
                pid_t relay_pid=fork();

                if(relay_pid<0){
                    _exit(1);
                }

                if(relay_pid==0){
                    close(output_pipe[1]);

                    int result=relay_output(output_pipe[0],command,output_fds);

                    for(size_t i=0;i<command->output_redirection_count;i++){
                        close(output_fds[i]);
                    }

                    free(output_fds);
                    _exit(result==0 ? 0 : 1);
                }
            }

            if(dup2(output_pipe[1],STDOUT_FILENO)<0){
                _exit(1);
            }

            close(output_pipe[1]);
        }

        execute_command(command);
        _exit(127);
    }

    if(background){
        int job=next_job_number++;

        if(add_background_process(pid,pid,job,command->argv[0])<0){
            kill(pid,SIGTERM);
        }

        printf("[%d] %d\n",job,(int)pid);
        fflush(stdout);
        restore_sigchld(&oldset);

        if(input_fd>=0){
            close(input_fd);
        }

        if(command->output_redirection_count>0){
            close(output_pipe[0]);
            close(output_pipe[1]);

            for(size_t i=0;i<command->output_redirection_count;i++){
                close(output_fds[i]);
            }

            free(output_fds);
        }

        return 0;
    }

    if(input_fd>=0){
        close(input_fd);
    }

    if(command->output_redirection_count>0){
        close(output_pipe[1]);

        if(relay_output(output_pipe[0],command,output_fds)<0){
            close(output_pipe[0]);

            for(size_t i=0;i<command->output_redirection_count;i++){
                close(output_fds[i]);
            }

            free(output_fds);
            (void)wait_for_pid(pid);
            return -1;
        }

        close(output_pipe[0]);

        for(size_t i=0;i<command->output_redirection_count;i++){
            close(output_fds[i]);
        }

        free(output_fds);
    }

    return wait_for_pid(pid);
}

/* Execute a pipeline. */

static int execute_pipeline(const Pipeline *pipeline){
    size_t command_count=pipeline->count;

    if(command_count==0){
        return -1;
    }

    if(command_count==1){
        return execute_simple_command(&pipeline->commands[0],pipeline->background);
    }

    size_t pipe_count=command_count-1;
    int(*pipes)[2]=malloc(pipe_count*sizeof(*pipes));
    pid_t *pids=malloc(command_count*sizeof(pid_t));

    if(pipes==NULL || pids==NULL){
        free(pipes);
        free(pids);
        perror("cshell: malloc");
        return -1;
    }

    sigset_t oldset;

    if(pipeline->background && block_sigchld(&oldset)<0){
        free(pipes);
        free(pids);
        perror("cshell: sigprocmask");
        return -1;
    }

    /* Create all pipes before forking. */
    for(size_t i=0;i<pipe_count;i++){
        if(pipe(pipes[i])<0){
            perror("cshell: pipe");

            for(size_t j=0;j<i;j++){
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if(pipeline->background){
                restore_sigchld(&oldset);
            }

            free(pipes);
            free(pids);
            return -1;
        }
    }

    /* Fork each pipeline stage. */
    for(size_t i=0;i<command_count;i++){
        pid_t pid=fork();

        if(pid<0){
            perror("cshell: fork");

            for(size_t j=0;j<pipe_count;j++){
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            for(size_t j=0;j<i;j++){
                (void)wait_for_pid(pids[j]);
            }

            if(pipeline->background){
                restore_sigchld(&oldset);
            }

            free(pipes);
            free(pids);
            return -1;
        }

        pids[i]=pid;

        pid_t pgid=i==0 ? pid : pids[0];

        if(setpgid(pid,pgid)<0 && errno!=EACCES && errno!=ESRCH){
            perror("cshell: setpgid");
        }

        if(pid==0){
            if(setpgid(0,pgid)<0){
                _exit(1);
            }
            const Command *command=&pipeline->commands[i];

            if(i>0){
                if(dup2(pipes[i-1][0],STDIN_FILENO)<0){
                    _exit(1);
                }
            }

            if(i<command_count-1){
                if(dup2(pipes[i][1],STDOUT_FILENO)<0){
                    _exit(1);
                }
            }

            /* Input redirection overrides the pipeline input. */
            if(command->input_redirection_count>0){
                if(setup_input_for_child(command)<0){
                    _exit(1);
                }
            }

            /* Output redirection overrides the pipeline output. */
            if(i==command_count-1 && command->output_redirection_count>0){
                int *output_fds=NULL;

                if(open_output_files(command,&output_fds)<0){
                    _exit(1);
                }

                int output_pipe[2];

                if(pipe(output_pipe)<0){
                    for(size_t j=0;j<command->output_redirection_count;j++){
                        close(output_fds[j]);
                    }

                    free(output_fds);
                    _exit(1);
                }

                pid_t relay_pid=fork();

                if(relay_pid<0){
                    close(output_pipe[0]);
                    close(output_pipe[1]);

                    for(size_t j=0;j<command->output_redirection_count;j++){
                        close(output_fds[j]);
                    }

                    free(output_fds);
                    _exit(1);
                }

                if(relay_pid==0){
                    close(output_pipe[1]);

                    int relay_result=relay_output(output_pipe[0],command,output_fds);

                    close(output_pipe[0]);

                    for(size_t j=0;j<command->output_redirection_count;j++){
                        close(output_fds[j]);
                    }

                    free(output_fds);
                    _exit(relay_result==0 ? 0 : 1);
                }

                close(output_pipe[0]);
                free(output_fds);

                if(dup2(output_pipe[1],STDOUT_FILENO)<0){
                    close(output_pipe[1]);
                    _exit(1);
                }

                close(output_pipe[1]);
            }

            for(size_t j=0;j<pipe_count;j++){
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if(pipeline->background){
                sigset_t set;
                sigemptyset(&set);
                sigprocmask(SIG_SETMASK,&set,NULL);
            }

            execute_command(command);
            _exit(127);
        }
    }

    /* Close pipes in the parent. */
    for(size_t i=0;i<pipe_count;i++){
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if(pipeline->background){
        int job=next_job_number++;

        for(size_t i=0;i<command_count;i++){
            if(add_background_process(pids[i],pids[0],job,pipeline->commands[i].argv[0])<0){
                kill(pids[i],SIGTERM);
            }
        }

        printf("[%d] %d\n",job,(int)pids[0]);
        fflush(stdout);
        restore_sigchld(&oldset);

        free(pipes);
        free(pids);
        return 0;
    }

    /* Wait for every foreground stage. */
    int result=0;

    for(size_t i=0;i<command_count;i++){
        if(wait_for_pid(pids[i])<0){
            result=-1;
        }
    }

    free(pipes);
    free(pids);

    return result;
}

/* Execute all command groups sequentially. */

int execute_command_list(const CommandList *command_list){
    if(command_list==NULL || command_list->count==0){
        return 0;
    }

    int result=0;

    for(size_t i=0;i<command_list->count;i++){
        if(execute_pipeline(&command_list->pipelines[i])<0){
            result=-1;
        }
    }

    return result;
}