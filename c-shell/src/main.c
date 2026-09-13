#include "shell.h"
#include "lexer.h"
#include "parser.h"
#include "execute.h"
#include "intrinsics.h"
#include <errno.h>

char shell_home[PATH_MAX];

void print_prompt(void){
    char cwd[PATH_MAX];
    char host[256];
    char *user=getenv("USER");

    if(getcwd(cwd,sizeof(cwd))==NULL){
        perror("getcwd error");
        return;
    }

    if(gethostname(host,sizeof(host))!=0){
        strcpy(host,"unknown");
    }

    size_t home_len=strlen(shell_home);

    if(strcmp(cwd,shell_home)==0){
        printf("<%s@%s:~> ",user ? user : "user",host);
    }else if(strncmp(cwd,shell_home,home_len)==0 && cwd[home_len]=='/'){
        printf("<%s@%s:~%s> ",user ? user : "user",host,cwd+home_len);
    }else{
        printf("<%s@%s:%s> ",user ? user : "user",host,cwd);
    }

    fflush(stdout);
}

int main(void){
    if(getcwd(shell_home,sizeof(shell_home))==NULL){
        perror("Fatal: could not get initial directory");
        return EXIT_FAILURE;
    }

    if(install_sigchld_handler()<0){
        perror("cshell: sigaction");
        return EXIT_FAILURE;
    }

    char input[SHELL_MAX_INPUT];

    while(1){
        print_prompt();

        if(fgets(input,sizeof(input),stdin)==NULL){
            printf("\n");
            break;
        }

        input[strcspn(input,"\n")]='\0';

        if(input[0]=='\0'){
            continue;
        }

        TokenList tokens;
        LexResult lex_result=lex_line(input,&tokens);

        if(lex_result==LEX_INVALID_SYNTAX){
            printf("cshell: invalid syntax\n");
            continue;
        }

        CommandList command_list;
        ParseResult parse_result=parse_tokens(&tokens,&command_list);

        if(parse_result==PARSE_INVALID_SYNTAX){
            printf("cshell: invalid syntax\n");
            free_tokens(&tokens);
            continue;
        }

        /* Part B */
        if(command_list.count>0 && command_list.pipelines[0].count==1 && !command_list.pipelines[0].background){
            Command *command=&command_list.pipelines[0].commands[0];

            if(command->argc>0 && is_intrinsic(command->argv[0])){
                (void)execute_intrinsic((int)command->argc,command->argv);

                free_command_list(&command_list);
                free_tokens(&tokens);
                continue;
            }
        }

        /* Execute command list. */
        (void)execute_command_list(&command_list);

        free_command_list(&command_list);
        free_tokens(&tokens);
    }

    return EXIT_SUCCESS;
}