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
#include <sys/ptrace.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
extern char **environ;
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
static pid_t shell_pgid=-1;
static int shell_terminal=-1;
static int job_control_enabled=0;
/* Restore default signal handling before a child is executed. */
static void set_default_signals(void){
    struct sigaction action;
    memset(&action,0,sizeof(action));
    action.sa_handler=SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);
    sigaction(SIGTSTP,&action,NULL);
    sigaction(SIGTTOU,&action,NULL);
    sigaction(SIGCHLD,&action,NULL);
}
/* Put the shell in its own process group and take terminal control. */
int initialize_job_control(void){
    shell_terminal=STDIN_FILENO;
    if(!isatty(shell_terminal)){
        return 0;
    }
    shell_pgid=getpid();
    if(setpgid(shell_pgid,shell_pgid)<0 && errno!=EACCES){
        return -1;
    }
    struct sigaction action;
    memset(&action,0,sizeof(action));
    action.sa_handler=SIG_IGN;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL)<0 || sigaction(SIGTSTP,&action,NULL)<0 || sigaction(SIGTTOU,&action,NULL)<0){
        return -1;
    }
    if(tcsetpgrp(shell_terminal,shell_pgid)<0){
        return -1;
    }
    job_control_enabled=1;
    return 0;
}
static void give_terminal_to(pid_t pgid){
    if(job_control_enabled){
        (void)tcsetpgrp(shell_terminal,pgid);
    }
}
static void reclaim_terminal(void){
    if(job_control_enabled){
        (void)tcsetpgrp(shell_terminal,shell_pgid);
    }
}
/* Check whether any tracked job is currently stopped. */
int has_stopped_jobs(void){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].stopped){
            return 1;
        }
    }
    return 0;
}
/* Send SIGHUP once to every tracked background process group. */
void send_sighup_to_jobs(void){
    pid_t groups[MAX_BACKGROUND];
    size_t count=0;
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(!background_processes[i].active || background_processes[i].pgid<=0){
            continue;
        }
        int found=0;
        for(size_t j=0;j<count;j++){
            if(groups[j]==background_processes[i].pgid){
                found=1;
                break;
            }
        }
        if(!found){
            groups[count++]=background_processes[i].pgid;
        }
    }
    for(size_t i=0;i<count;i++){
        (void)kill(-groups[i],SIGHUP);
    }
}
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
/* Reap background children and update their job states. */
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
            result=waitpid(background_processes[i].pid, &status, WNOHANG|WUNTRACED|WCONTINUED);
            if(result==background_processes[i].pid){
                if(WIFEXITED(status) || WIFSIGNALED(status)){
                    background_message(background_processes[i].command, result, WIFEXITED(status));
                    background_processes[i].active=0;
                }else if(WIFSTOPPED(status)){
                    background_processes[i].stopped=1;
                }else if(WIFCONTINUED(status)){
                    background_processes[i].stopped=0;
                }
            }
        }while(result==background_processes[i].pid && background_processes[i].active);
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
/* Print tracked jobs grouped by their process group. */
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
        printf("[%d] pgid %d\n", group->job_number, (int)group->pgid);
        for(size_t j=0;j<MAX_BACKGROUND;j++){
            if(!background_processes[j].active || background_processes[j].job_number!=group->job_number){
                continue;
            }
            printf("  %d %s %s\n", (int)background_processes[j].pid, background_processes[j].command, background_processes[j].stopped ? "Stopped" : "Running");
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
/* Combine input redirections into one input stream. */
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
/* Open every output redirection requested by a command. */
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
/* Resolve a command and replace the child with it. */
static void execute_command(const Command *command){
    const char *original=command->argv[0];
    if(original==NULL || original[0]=='\0'){
        _exit(127);
    }
    if(is_intrinsic(original)){
        int result=execute_intrinsic((int)command->argc,command->argv);
        _exit(result==0 ? 0 : 1);
    }
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
    if(strchr(original,'/')!=NULL){
        if(access(original,X_OK)==0){
            execv(original,command->argv);
        }
        fprintf(stderr,"cshell: command not found(%s)\n",original);
        _exit(127);
    }
    char local_path[PATH_MAX];
    int written=snprintf(local_path,sizeof(local_path),"./%s",original);
    if(written>=0 && (size_t)written<sizeof(local_path) && is_regular_executable(local_path)){
        execv(local_path,command->argv);
    }
    execvp(original,command->argv);
    fprintf(stderr,"cshell: command not found(%s)\n",original);
    _exit(127);
}
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
static int add_stopped_process(pid_t pid,pid_t pgid,int job_number,const char *command){
    if(add_background_process(pid,pgid,job_number,command)<0){
        return -1;
    }
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].pid==pid && background_processes[i].job_number==job_number){
            background_processes[i].stopped=1;
            return 0;
        }
    }
    return -1;
}
static int wait_foreground_process(pid_t pid,pid_t pgid,int job_number,const char *command){
    int status;
    while(1){
        pid_t result=waitpid(pid,&status,WUNTRACED);
        if(result==pid){
            if(WIFSTOPPED(status)){
                return add_stopped_process(pid,pgid,job_number,command);
            }
            return 0;
        }
        if(result<0 && errno==EINTR){
            continue;
        }
        if(result<0){
            perror("cshell: waitpid");
            return -1;
        }
    }
}
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
        set_default_signals();
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
    int job=next_job_number++;
    give_terminal_to(pid);
    if(command->output_redirection_count>0){
        close(output_pipe[1]);
        if(relay_output(output_pipe[0],command,output_fds)<0){
            close(output_pipe[0]);
            for(size_t i=0;i<command->output_redirection_count;i++){
                close(output_fds[i]);
            }
            free(output_fds);
            (void)wait_foreground_process(pid,pid,job,command->argv[0]);
            reclaim_terminal();
            return -1;
        }
        close(output_pipe[0]);
        for(size_t i=0;i<command->output_redirection_count;i++){
            close(output_fds[i]);
        }
        free(output_fds);
    }
    int result=wait_foreground_process(pid,pid,job,command->argv[0]);
    reclaim_terminal();
    if(has_stopped_jobs()){
        printf("\n[%d] + Stopped %s\n", job,command->argv[0]);
        fflush(stdout);
    }
    return result;
}
/* Create the pipeline, process groups and foreground/background job. */
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
            set_default_signals();
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
            if(command->input_redirection_count>0){
                if(setup_input_for_child(command)<0){
                    _exit(1);
                }
            }
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
    int job=next_job_number++;
    give_terminal_to(pids[0]);
    int result=0;
    for(size_t i=0;i<command_count;i++){
        if(wait_foreground_process(pids[i],pids[0],job,pipeline->commands[i].argv[0])<0){
            result=-1;
        }
    }
    reclaim_terminal();
    if(has_stopped_jobs()){
        printf("\n[%d] + Stopped %s\n", job,pipeline->commands[0].argv[0]);
        fflush(stdout);
    }
    free(pipes);
    free(pids);
    return result;
}
/* Execute pipelines in the order in which they were entered. */
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
static volatile sig_atomic_t resume_timed_out=0;
static pid_t resume_timeout_pgid=-1;
/* Terminate the resumed job when its foreground timeout expires. */
static void resume_timeout_handler(int signal){
    (void)signal;
    resume_timed_out=1;
    if(resume_timeout_pgid>0){
        (void)kill(-resume_timeout_pgid,SIGTERM);
    }
}
static BackgroundProcess *find_job(int job_number){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].job_number==job_number){
            return &background_processes[i];
        }
    }
    return NULL;
}
static void mark_job_running(int job_number){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].job_number==job_number){
            background_processes[i].stopped=0;
        }
    }
}
static int wait_resumed_job(int job_number,pid_t pgid,int timeout){
    int status;
    int remaining=0;
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].job_number==job_number){
            remaining++;
        }
    }
    resume_timed_out=0;
    resume_timeout_pgid=timeout>=0 ? pgid : -1;
    if(timeout>=0){
        struct sigaction action;
        memset(&action,0,sizeof(action));
        action.sa_handler=resume_timeout_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags=SA_RESTART;
        if(sigaction(SIGALRM,&action,NULL)<0){
            resume_timeout_pgid=-1;
            return -1;
        }
        alarm((unsigned int)timeout);
    }
    while(remaining>0){
        pid_t result=waitpid(-pgid,&status,WUNTRACED);
        if(result>0){
            for(size_t i=0;i<MAX_BACKGROUND;i++){
                if(!background_processes[i].active || background_processes[i].job_number!=job_number || background_processes[i].pid!=result){
                    continue;
                }
                if(WIFSTOPPED(status)){
                    background_processes[i].stopped=1;
                    remaining--;
                }else if(WIFEXITED(status) || WIFSIGNALED(status)){
                    background_processes[i].active=0;
                    remaining--;
                }
                break;
            }
            continue;
        }
        if(result<0 && errno==EINTR){
            if(resume_timed_out){
                continue;
            }
            continue;
        }
        if(result<0 && errno==ECHILD){
            break;
        }
        if(result<0){
            if(timeout>=0){
                alarm(0);
                resume_timeout_pgid=-1;
            }
            return -1;
        }
    }
    if(timeout>=0){
        alarm(0);
        resume_timeout_pgid=-1;
    }
    return resume_timed_out ? 1 : 0;
}
/* Continue a stopped job in the foreground or background. */
int execute_resume(int argc,char *const argv[]){
    if(argc<3 || argc>5 || argv==NULL || argv[1]==NULL || argv[2]==NULL){
        fprintf(stderr,"resume: invalid syntax\n");
        return -1;
    }
    const char *job_text=argv[1];
    if(job_text[0]!='%' || job_text[1]=='\0'){
        fprintf(stderr,"resume: invalid syntax\n");
        return -1;
    }
    char *end=NULL;
    long job_value=strtol(job_text+1,&end,10);
    if(*end!='\0' || job_value<=0 || job_value>INT_MAX){
        fprintf(stderr,"resume: invalid syntax\n");
        return -1;
    }
    int job_number=(int)job_value;
    const char *mode=argv[2];
    int timeout=-1;
    if(strcmp(mode,"bg")==0){
        if(argc!=3){
            fprintf(stderr,"resume: invalid syntax\n");
            return -1;
        }
    }else if(strcmp(mode,"fg")==0){
        if(argc==5){
            if(strcmp(argv[3],"--timeout")!=0 || argv[4]==NULL || argv[4][0]=='\0'){
                fprintf(stderr,"resume: invalid syntax\n");
                return -1;
            }
            char *timeout_end=NULL;
            long timeout_value=strtol(argv[4],&timeout_end,10);
            if(*timeout_end!='\0' || timeout_value<0 || timeout_value>UINT_MAX){
                fprintf(stderr,"resume: invalid syntax\n");
                return -1;
            }
            timeout=(int)timeout_value;
        }else if(argc!=3){
            fprintf(stderr,"resume: invalid syntax\n");
            return -1;
        }
    }else{
        fprintf(stderr,"resume: invalid syntax\n");
        return -1;
    }
    sigset_t oldset;
    if(block_sigchld(&oldset)<0){
        return -1;
    }
    cleanup_background_processes();
    BackgroundProcess *job=find_job(job_number);
    if(job==NULL){
        restore_sigchld(&oldset);
        fprintf(stderr,"resume: no such job\n");
        return -1;
    }
    pid_t pgid=job->pgid;
    char command[PATH_MAX];
    strncpy(command,job->command,PATH_MAX-1);
    command[PATH_MAX-1]='\0';
    if(kill(-pgid,SIGCONT)<0){
        restore_sigchld(&oldset);
        fprintf(stderr,"resume: no such job\n");
        return -1;
    }
    mark_job_running(job_number);
    restore_sigchld(&oldset);
    if(strcmp(mode,"bg")==0){
        printf("[%d] + Running %s\n",job_number,command);
        fflush(stdout);
        return 0;
    }
    printf("%s\n",command);
    fflush(stdout);
    give_terminal_to(pgid);
    int result=wait_resumed_job(job_number,pgid,timeout);
    reclaim_terminal();
    if(result<0){
        return -1;
    }
    if(result==1){
        printf("resume: job timed out\n");
        fflush(stdout);
        return 0;
    }
    if(find_job(job_number)!=NULL && has_stopped_jobs()){
        printf("\n[%d] + Stopped %s\n", job_number,command);
        fflush(stdout);
    }
    return 0;
}
static BackgroundProcess *find_process(pid_t pid){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].pid==pid){
            return &background_processes[i];
        }
    }
    return NULL;
}
static BackgroundProcess *find_job_for_ping(int job_number){
    for(size_t i=0;i<MAX_BACKGROUND;i++){
        if(background_processes[i].active && background_processes[i].job_number==job_number){
            return &background_processes[i];
        }
    }
    return NULL;
}
/* Send a signal to a tracked process or process group. */
int execute_ping(int argc,char *const argv[]){
    if(argc!=3 || argv[1]==NULL || argv[2]==NULL){
        fprintf(stderr,"ping: invalid syntax\n");
        return -1;
    }
    char *signal_end=NULL;
    long signal_value=strtol(argv[2],&signal_end,10);
    if(argv[2][0]=='-' || argv[2][0]=='\0' || *signal_end!='\0' || signal_value<0){
        fprintf(stderr,"ping: invalid syntax\n");
        return -1;
    }
    int signal_number=(int)(signal_value%64);
    sigset_t oldset;
    if(block_sigchld(&oldset)<0){
        return -1;
    }
    cleanup_background_processes();
    if(argv[1][0]=='%'){
        char *job_end=NULL;
        long job_value=strtol(argv[1]+1,&job_end,10);
        if(argv[1][1]=='\0' || *job_end!='\0' || job_value<=0 || job_value>INT_MAX){
            restore_sigchld(&oldset);
            fprintf(stderr,"ping: no such process found\n");
            return -1;
        }
        BackgroundProcess *job= find_job_for_ping((int)job_value);
        if(job==NULL){
            restore_sigchld(&oldset);
            fprintf(stderr,"ping: no such process found\n");
            return -1;
        }
        pid_t pgid=job->pgid;
        if(kill(-pgid,signal_number)<0){
            restore_sigchld(&oldset);
            fprintf(stderr,"ping: no such process found\n");
            return -1;
        }
        restore_sigchld(&oldset);
        printf("Sent signal %ld to %s\n",signal_value,argv[1]);
        fflush(stdout);
        return 0;
    }
    char *pid_end=NULL;
    long pid_value=strtol(argv[1],&pid_end,10);
    if(argv[1][0]=='\0' || *pid_end!='\0' || pid_value<=0 || pid_value>INT_MAX){
        restore_sigchld(&oldset);
        fprintf(stderr,"ping: no such process found\n");
        return -1;
    }
    BackgroundProcess *process= find_process((pid_t)pid_value);
    if(process==NULL){
        restore_sigchld(&oldset);
        fprintf(stderr,"ping: no such process found\n");
        return -1;
    }
    if(kill(process->pid,signal_number)<0){
        restore_sigchld(&oldset);
        fprintf(stderr,"ping: no such process found\n");
        return -1;
    }
    restore_sigchld(&oldset);
    printf("Sent signal %ld to %s\n",signal_value,argv[1]);
    fflush(stdout);
    return 0;
}
typedef struct{
    long number;
    unsigned long calls;
    double total_time;
    unsigned long first_order;
} SnoopStat;
static const char *snoop_syscall_names[463]={
    [0]="read", [1]="write", [2]="open", [3]="close", [4]="stat", [5]="fstat", [6]="lstat", [7]="poll", [8]="lseek", [9]="mmap", [10]="mprotect", [11]="munmap", [12]="brk", [13]="rt_sigaction", [14]="rt_sigprocmask", [15]="rt_sigreturn", [16]="ioctl", [17]="pread64", [18]="pwrite64", [19]="readv", [20]="writev", [21]="access", [22]="pipe", [23]="select", [24]="sched_yield", [25]="mremap", [26]="msync", [27]="mincore", [28]="madvise", [29]="shmget", [30]="shmat", [31]="shmctl", [32]="dup", [33]="dup2", [34]="pause", [35]="nanosleep", [36]="getitimer", [37]="alarm", [38]="setitimer", [39]="getpid", [40]="sendfile", [41]="socket", [42]="connect", [43]="accept", [44]="sendto", [45]="recvfrom", [46]="sendmsg", [47]="recvmsg", [48]="shutdown", [49]="bind", [50]="listen", [51]="getsockname", [52]="getpeername", [53]="socketpair", [54]="setsockopt", [55]="getsockopt", [56]="clone", [57]="fork", [58]="vfork", [59]="execve", [60]="exit", [61]="wait4", [62]="kill", [63]="uname", [64]="semget", [65]="semop", [66]="semctl", [67]="shmdt", [68]="msgget", [69]="msgsnd", [70]="msgrcv", [71]="msgctl", [72]="fcntl", [73]="flock", [74]="fsync", [75]="fdatasync", [76]="truncate", [77]="ftruncate", [78]="getdents", [79]="getcwd", [80]="chdir", [81]="fchdir", [82]="rename", [83]="mkdir", [84]="rmdir", [85]="creat", [86]="link", [87]="unlink", [88]="symlink", [89]="readlink", [90]="chmod", [91]="fchmod", [92]="chown", [93]="fchown", [94]="lchown", [95]="umask", [96]="gettimeofday", [97]="getrlimit", [98]="getrusage", [99]="sysinfo", [100]="times", [101]="ptrace", [102]="getuid", [103]="syslog", [104]="getgid", [105]="setuid", [106]="setgid", [107]="geteuid", [108]="getegid", [109]="setpgid", [110]="getppid", [111]="getpgrp", [112]="setsid", [113]="setreuid", [114]="setregid", [115]="getgroups", [116]="setgroups", [117]="setresuid", [118]="getresuid", [119]="setresgid", [120]="getresgid", [121]="getpgid", [122]="setfsuid", [123]="setfsgid", [124]="getsid", [125]="capget", [126]="capset", [127]="rt_sigpending", [128]="rt_sigtimedwait", [129]="rt_sigqueueinfo", [130]="rt_sigsuspend", [131]="sigaltstack", [132]="utime", [133]="mknod", [134]="uselib", [135]="personality", [136]="ustat", [137]="statfs", [138]="fstatfs", [139]="sysfs", [140]="getpriority", [141]="setpriority", [142]="sched_setparam", [143]="sched_getparam", [144]="sched_setscheduler", [145]="sched_getscheduler", [146]="sched_get_priority_max", [147]="sched_get_priority_min", [148]="sched_rr_get_interval", [149]="mlock", [150]="munlock", [151]="mlockall", [152]="munlockall", [153]="vhangup", [154]="modify_ldt", [155]="pivot_root", [156]="_sysctl", [157]="prctl", [158]="arch_prctl", [159]="adjtimex", [160]="setrlimit", [161]="chroot", [162]="sync", [163]="acct", [164]="settimeofday", [165]="mount", [166]="umount2", [167]="swapon", [168]="swapoff", [169]="reboot", [170]="sethostname", [171]="setdomainname", [172]="iopl", [173]="ioperm", [174]="create_module", [175]="init_module", [176]="delete_module", [177]="get_kernel_syms", [178]="query_module", [179]="quotactl", [180]="nfsservctl", [181]="getpmsg", [182]="putpmsg", [183]="afs_syscall", [184]="tuxcall", [185]="security", [186]="gettid", [187]="readahead", [188]="setxattr", [189]="lsetxattr", [190]="fsetxattr", [191]="getxattr", [192]="lgetxattr", [193]="fgetxattr", [194]="listxattr", [195]="llistxattr", [196]="flistxattr", [197]="removexattr", [198]="lremovexattr", [199]="fremovexattr", [200]="tkill", [201]="time", [202]="futex", [203]="sched_setaffinity", [204]="sched_getaffinity", [205]="set_thread_area", [206]="io_setup", [207]="io_destroy", [208]="io_getevents", [209]="io_submit", [210]="io_cancel", [211]="get_thread_area", [212]="lookup_dcookie", [213]="epoll_create", [214]="epoll_ctl_old", [215]="epoll_wait_old", [216]="remap_file_pages", [217]="getdents64", [218]="set_tid_address", [219]="restart_syscall", [220]="semtimedop", [221]="fadvise64", [222]="timer_create", [223]="timer_settime", [224]="timer_gettime", [225]="timer_getoverrun", [226]="timer_delete", [227]="clock_settime", [228]="clock_gettime", [229]="clock_getres", [230]="clock_nanosleep", [231]="exit_group", [232]="epoll_wait", [233]="epoll_ctl", [234]="tgkill", [235]="utimes", [236]="vserver", [237]="mbind", [238]="set_mempolicy", [239]="get_mempolicy", [240]="mq_open", [241]="mq_unlink", [242]="mq_timedsend", [243]="mq_timedreceive", [244]="mq_notify", [245]="mq_getsetattr", [246]="kexec_load", [247]="waitid", [248]="add_key", [249]="request_key", [250]="keyctl", [251]="ioprio_set", [252]="ioprio_get", [253]="inotify_init", [254]="inotify_add_watch", [255]="inotify_rm_watch", [256]="migrate_pages", [257]="openat", [258]="mkdirat", [259]="mknodat", [260]="fchownat", [261]="futimesat", [262]="newfstatat", [263]="unlinkat", [264]="renameat", [265]="linkat", [266]="symlinkat", [267]="readlinkat", [268]="fchmodat", [269]="faccessat", [270]="pselect6", [271]="ppoll", [272]="unshare", [273]="set_robust_list", [274]="get_robust_list", [275]="splice", [276]="tee", [277]="sync_file_range", [278]="vmsplice", [279]="move_pages", [280]="utimensat", [281]="epoll_pwait", [282]="signalfd", [283]="timerfd_create", [284]="eventfd", [285]="fallocate", [286]="timerfd_settime", [287]="timerfd_gettime", [288]="accept4", [289]="signalfd4", [290]="eventfd2", [291]="epoll_create1", [292]="dup3", [293]="pipe2", [294]="inotify_init1", [295]="preadv", [296]="pwritev", [297]="rt_tgsigqueueinfo", [298]="perf_event_open", [299]="recvmmsg", [300]="fanotify_init", [301]="fanotify_mark", [302]="prlimit64", [303]="name_to_handle_at", [304]="open_by_handle_at", [305]="clock_adjtime", [306]="syncfs", [307]="sendmmsg", [308]="setns", [309]="getcpu", [310]="process_vm_readv", [311]="process_vm_writev", [312]="kcmp", [313]="finit_module", [314]="sched_setattr", [315]="sched_getattr", [316]="renameat2", [317]="seccomp", [318]="getrandom", [319]="memfd_create", [320]="kexec_file_load", [321]="bpf", [322]="execveat", [323]="userfaultfd", [324]="membarrier", [325]="mlock2", [326]="copy_file_range", [327]="preadv2", [328]="pwritev2", [329]="pkey_mprotect", [330]="pkey_alloc", [331]="pkey_free", [332]="statx", [333]="io_pgetevents", [334]="rseq", [335]="uretprobe", [424]="pidfd_send_signal", [425]="io_uring_setup", [426]="io_uring_enter", [427]="io_uring_register", [428]="open_tree", [429]="move_mount", [430]="fsopen", [431]="fsconfig", [432]="fsmount", [433]="fspick", [434]="pidfd_open", [435]="clone3", [436]="close_range", [437]="openat2", [438]="pidfd_getfd", [439]="faccessat2", [440]="process_madvise", [441]="epoll_pwait2", [442]="mount_setattr", [443]="quotactl_fd", [444]="landlock_create_ruleset", [445]="landlock_add_rule", [446]="landlock_restrict_self", [447]="memfd_secret", [448]="process_mrelease", [449]="futex_waitv", [450]="set_mempolicy_home_node", [451]="cachestat", [452]="fchmodat2", [453]="map_shadow_stack", [454]="futex_wake", [455]="futex_wait", [456]="futex_requeue", [457]="statmount", [458]="listmount", [459]="lsm_get_self_attr", [460]="lsm_set_self_attr", [461]="lsm_list_modules", [462]="mseal", };
static const char *snoop_syscall_name(long number){
    static char unknown[32];
    if(number>=0 && number<(long)(sizeof(snoop_syscall_names)/ sizeof(snoop_syscall_names[0])) && snoop_syscall_names[number]!=NULL){
        return snoop_syscall_names[number];
    }
    snprintf(unknown,sizeof(unknown),"syscall_%ld",number);
    return unknown;
}
static double snoop_time_difference(const struct timespec *start, const struct timespec *end){
    return (double)(end->tv_sec-start->tv_sec)+ (double)(end->tv_nsec-start->tv_nsec)/1000000000.0;
}
static int snoop_add_stat(SnoopStat **stats,size_t *count,size_t *capacity, long number,double elapsed,unsigned long order){
    for(size_t i=0;i<*count;i++){
        if((*stats)[i].number==number){
            (*stats)[i].calls++;
            (*stats)[i].total_time+=elapsed;
            return 0;
        }
    }
    if(*count>=*capacity){
        size_t new_capacity=*capacity==0 ? 32 : *capacity*2;
        SnoopStat *new_stats=realloc(*stats, new_capacity*sizeof(SnoopStat));
        if(new_stats==NULL){
            return -1;
        }
        *stats=new_stats;
        *capacity=new_capacity;
    }
    (*stats)[*count].number=number;
    (*stats)[*count].calls=1;
    (*stats)[*count].total_time=elapsed;
    (*stats)[*count].first_order=order;
    (*count)++;
    return 0;
}
static int snoop_compare_stats(const void *a,const void *b){
    const SnoopStat *left=(const SnoopStat *)a;
    const SnoopStat *right=(const SnoopStat *)b;
    if(left->calls<right->calls){
        return 1;
    }
    if(left->calls>right->calls){
        return -1;
    }
    if(left->first_order<right->first_order){
        return -1;
    }
    if(left->first_order>right->first_order){
        return 1;
    }
    return 0;
}
static int snoop_join_path(char *result,size_t result_size, const char *directory,const char *name){
    size_t directory_length=strlen(directory);
    size_t name_length=strlen(name);
    int separator=directory_length>0 && directory[directory_length-1]!='/';
    if(directory_length+name_length+(size_t)separator+1>result_size){
        return -1;
    }
    memcpy(result,directory,directory_length);
    size_t offset=directory_length;
    if(separator){
        result[offset++]='/';
    }
    memcpy(result+offset,name,name_length);
    result[offset+name_length]='\0';
    return 0;
}
static int snoop_resolve_command(const char *command, char *path,size_t path_size){
    if(command==NULL || command[0]=='\0'){
        return -1;
    }
    if(strchr(command,'/')!=NULL){
        if(access(command,X_OK)!=0 || strlen(command)>=path_size){
            return -1;
        }
        snprintf(path,path_size,"%s",command);
        return 0;
    }
    char local_path[PATH_MAX];
    int written=snprintf(local_path,sizeof(local_path),"./%s",command);
    if(written>=0 && (size_t)written<sizeof(local_path) && access(local_path,X_OK)==0){
        if(realpath(local_path,path)==NULL){
            if(strlen(local_path)>=path_size){
                return -1;
            }
            snprintf(path,path_size,"%s",local_path);
        }
        return 0;
    }
    const char *path_env=getenv("PATH");
    if(path_env==NULL){
        return -1;
    }
    char *path_copy=strdup(path_env);
    if(path_copy==NULL){
        return -1;
    }
    char *saveptr=NULL;
    char *directory=strtok_r(path_copy,":",&saveptr);
    while(directory!=NULL){
        const char *dir=directory[0]=='\0' ? "." : directory;
        char candidate[PATH_MAX];
        if(snoop_join_path(candidate,sizeof(candidate),dir,command)==0 && access(candidate,X_OK)==0){
            if(realpath(candidate,path)==NULL){
                if(strlen(candidate)>=path_size){
                    free(path_copy);
                    return -1;
                }
                snprintf(path,path_size,"%s",candidate);
            }
            free(path_copy);
            return 0;
        }
        directory=strtok_r(NULL,":",&saveptr);
    }
    free(path_copy);
    return -1;
}
static int snoop_wait_for_stop(pid_t pid){
    int status;
    while(waitpid(pid,&status,0)<0){
        if(errno==EINTR){
            continue;
        }
        return -1;
    }
    return WIFSTOPPED(status) ? 0 : -1;
}
/* Trace syscall entry and exit stops and collect their timings. */
static int snoop_trace(pid_t pid){
    SnoopStat *stats=NULL;
    size_t stat_count=0;
    size_t stat_capacity=0;
    unsigned long order=0;
    long current_syscall=-1;
    struct timespec entry_time;
    int in_syscall=0;
    int status;
    if(snoop_wait_for_stop(pid)<0){
        free(stats);
        return -1;
    }
    if(ptrace(PTRACE_SETOPTIONS,pid,0, (void *)(long)PTRACE_O_TRACESYSGOOD)<0){
        free(stats);
        return -1;
    }
    if(ptrace(PTRACE_SYSCALL,pid,0,0)<0){
        free(stats);
        return -1;
    }
    while(1){
        pid_t result;
        do{
            result=waitpid(pid,&status,0);
        }while(result<0 && errno==EINTR);
        if(result<0){
            free(stats);
            return -1;
        }
        if(WIFEXITED(status) || WIFSIGNALED(status)){
            break;
        }
        if(!WIFSTOPPED(status)){
            continue;
        }
        int stop_signal=WSTOPSIG(status);
        if(stop_signal==(SIGTRAP|0x80)){
            struct user_regs_struct regs;
            if(ptrace(PTRACE_GETREGS,pid,0,&regs)<0){
                free(stats);
                return -1;
            }
            if(!in_syscall){
                current_syscall=(long)regs.orig_rax;
                if(clock_gettime(CLOCK_MONOTONIC,&entry_time)<0){
                    free(stats);
                    return -1;
                }
                in_syscall=1;
            }else{
                struct timespec exit_time;
                if(clock_gettime(CLOCK_MONOTONIC,&exit_time)<0){
                    free(stats);
                    return -1;
                }
                if(snoop_add_stat(&stats,&stat_count,&stat_capacity, current_syscall, snoop_time_difference(&entry_time, &exit_time), order++)<0){
                    free(stats);
                    return -1;
                }
                in_syscall=0;
                current_syscall=-1;
            }
            if(ptrace(PTRACE_SYSCALL,pid,0,0)<0){
                free(stats);
                return -1;
            }
            continue;
        }
        if(stop_signal==SIGTRAP){
            if(ptrace(PTRACE_SYSCALL,pid,0,0)<0){
                free(stats);
                return -1;
            }
            continue;
        }
        if(ptrace(PTRACE_SYSCALL,pid,0,stop_signal)<0){
            free(stats);
            return -1;
        }
    }
    qsort(stats,stat_count,sizeof(SnoopStat),snoop_compare_stats);
    printf("syscall       calls   time\n");
    for(size_t i=0;i<stat_count;i++){
        printf("%-13s %-7lu %.3fs\n", snoop_syscall_name(stats[i].number), stats[i].calls, stats[i].total_time);
    }
    free(stats);
    return 0;
}
/* Start a command under ptrace and print its syscall summary. */
static int execute_snoop_command(int argc,char *const argv[]){
    char command_path[PATH_MAX];
    if(argc<2){
        fprintf(stderr,"snoop: invalid syntax\n");
        return -1;
    }
    if(snoop_resolve_command(argv[1],command_path, sizeof(command_path))<0){
        fprintf(stderr,"snoop: command not found\n");
        return -1;
    }
    pid_t pid=fork();
    if(pid<0){
        return -1;
    }
    if(pid==0){
        set_default_signals();
        if(ptrace(PTRACE_TRACEME,0,0,0)<0){
            _exit(127);
        }
        raise(SIGSTOP);
        execve(command_path,argv+1,environ);
        _exit(127);
    }
    if(snoop_trace(pid)<0){
        (void)kill(pid,SIGKILL);
        while(waitpid(pid,NULL,0)<0 && errno==EINTR){
        }
        return -1;
    }
    return 0;
}
/* Attach to an existing process and trace its syscalls. */
static int execute_snoop_pid(const char *pid_text){
    char *end=NULL;
    long value=strtol(pid_text,&end,10);
    if(pid_text[0]=='\0' || *end!='\0' || value<=0 || value>INT_MAX){
        fprintf(stderr,"snoop: no such process\n");
        return -1;
    }
    pid_t pid=(pid_t)value;
    if(kill(pid,0)<0 && errno!=EPERM){
        fprintf(stderr,"snoop: no such process\n");
        return -1;
    }
    if(ptrace(PTRACE_ATTACH,pid,0,0)<0){
        fprintf(stderr,"snoop: no such process\n");
        return -1;
    }
    if(snoop_trace(pid)<0){
        (void)ptrace(PTRACE_DETACH,pid,0,0);
        return -1;
    }
    return 0;
}
/* Parse snoop arguments and select command or PID tracing. */
int execute_snoop(int argc,char *const argv[]){
    if(argc<2){
        fprintf(stderr,"snoop: invalid syntax\n");
        return -1;
    }
    if(strcmp(argv[1],"-p")==0){
        if(argc!=3){
            fprintf(stderr,"snoop: invalid syntax\n");
            return -1;
        }
        return execute_snoop_pid(argv[2]);
    }
    return execute_snoop_command(argc,argv);
}
static const char *spy_file_type(const char *path){
    struct stat st;
    if(stat(path,&st)<0){
        return "UNKNOWN";
    }
    if(S_ISREG(st.st_mode)){
        return "REG";
    }
    if(S_ISDIR(st.st_mode)){
        return "DIR";
    }
    if(S_ISCHR(st.st_mode)){
        return "CHR";
    }
    if(S_ISBLK(st.st_mode)){
        return "BLK";
    }
    if(S_ISFIFO(st.st_mode)){
        return "FIFO";
    }
    if(S_ISSOCK(st.st_mode)){
        return "SOCK";
    }
    return "UNKNOWN";
}
static void spy_print_entry(pid_t pid,const char *fd,const char *path){
    printf("%d    %s    %s    %s\n", (int)pid,fd,spy_file_type(path),path);
}
static int spy_read_link(pid_t pid,const char *name, char *path,size_t path_size){
    char proc_path[PATH_MAX];
    int written=snprintf(proc_path,sizeof(proc_path), "/proc/%d/%s",(int)pid,name);
    if(written<0 || (size_t)written>=sizeof(proc_path)){
        return -1;
    }
    ssize_t length=readlink(proc_path,path,path_size-1);
    if(length<0){
        return -1;
    }
    path[length]='\0';
    return 0;
}
static int spy_print_memory(pid_t pid){
    char maps_path[PATH_MAX];
    snprintf(maps_path,sizeof(maps_path), "/proc/%d/maps",(int)pid);
    FILE *file=fopen(maps_path,"r");
    if(file==NULL){
        return -1;
    }
    char line[PATH_MAX*2];
    char **seen=NULL;
    size_t seen_count=0;
    size_t seen_capacity=0;
    while(fgets(line,sizeof(line),file)!=NULL){
        unsigned long start;
        unsigned long end;
        unsigned long offset;
        unsigned int dev_major;
        unsigned int dev_minor;
        unsigned long inode;
        char permissions[8];
        char path[PATH_MAX];
        int fields=sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %4095[^\n]", &start,&end,permissions,&offset, &dev_major,&dev_minor,&inode,path);
        if(fields<7){
            continue;
        }
        if(fields<8){
            continue;
        }
        char *mapped_path=path;
        while(*mapped_path==' ' || *mapped_path=='\t'){
            mapped_path++;
        }
        if(mapped_path[0]!='/' || mapped_path[0]=='\0'){
            continue;
        }
        int duplicate=0;
        for(size_t i=0;i<seen_count;i++){
            if(strcmp(seen[i],mapped_path)==0){
                duplicate=1;
                break;
            }
        }
        if(duplicate){
            continue;
        }
        if(seen_count==seen_capacity){
            size_t new_capacity=seen_capacity==0 ? 32 : seen_capacity*2;
            char **new_seen=realloc(seen, new_capacity*sizeof(char *));
            if(new_seen==NULL){
                for(size_t i=0;i<seen_count;i++){
                    free(seen[i]);
                }
                free(seen);
                fclose(file);
                return -1;
            }
            seen=new_seen;
            seen_capacity=new_capacity;
        }
        seen[seen_count]=strdup(mapped_path);
        if(seen[seen_count]==NULL){
            for(size_t i=0;i<seen_count;i++){
                free(seen[i]);
            }
            free(seen);
            fclose(file);
            return -1;
        }
        seen_count++;
        spy_print_entry(pid,"mem",mapped_path);
    }
    for(size_t i=0;i<seen_count;i++){
        free(seen[i]);
    }
    free(seen);
    fclose(file);
    return 0;
}
/* Inspect the open files and mappings of a process. */
int execute_spy(int argc,char *const argv[]){
    if(argc>2){
        fprintf(stderr,"spy: invalid syntax\n");
        return -1;
    }
    pid_t pid=getpid();
    if(argc==2){
        char *end=NULL;
        long value=strtol(argv[1],&end,10);
        if(argv[1][0]=='\0' || *end!='\0' || value<=0 || value>INT_MAX){
            fprintf(stderr,"spy: no such process\n");
            return -1;
        }
        pid=(pid_t)value;
    }
    char proc_path[PATH_MAX];
    snprintf(proc_path,sizeof(proc_path),"/proc/%d",(int)pid);
    struct stat st;
    if(stat(proc_path,&st)<0 || !S_ISDIR(st.st_mode)){
        fprintf(stderr,"spy: no such process\n");
        return -1;
    }
    printf("PID    FD    TYPE    PATH\n");
    char path[PATH_MAX];
    if(spy_read_link(pid,"cwd",path,sizeof(path))==0){
        spy_print_entry(pid,"cwd",path);
    }
    if(spy_read_link(pid,"exe",path,sizeof(path))==0){
        spy_print_entry(pid,"txt",path);
    }
    spy_print_memory(pid);
    char fd_path[PATH_MAX];
    snprintf(fd_path,sizeof(fd_path), "/proc/%d/fd",(int)pid);
    DIR *directory=opendir(fd_path);
    if(directory==NULL){
        return 0;
    }
    struct dirent *entry;
    while((entry=readdir(directory))!=NULL){
        if(entry->d_name[0]=='.'){
            continue;
        }
        char link_path[PATH_MAX];
        snprintf(link_path,sizeof(link_path), "fd/%s",entry->d_name);
        if(spy_read_link(pid,link_path,path,sizeof(path))==0){
            spy_print_entry(pid,entry->d_name,path);
        }
    }
    closedir(directory);
    return 0;
}
