#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <ctype.h>
#define PATH_MAX 4096
#define MSG_SIZE 4096
#define BUF_SIZE 4096


static volatile sig_atomic_t timer_cancelled = 0;

static void timer_sigint_handler(int signo) {
    (void)signo;
    timer_cancelled = 1;
}

const char *sysname = "shellish";

enum return_codes {
  SUCCESS = 0,
  EXIT = 1,
  UNKNOWN = 2,
};

// change

struct command_t {
  char *name;
  bool background;
  bool auto_complete;
  int arg_count;
  char **args;
  char *redirects[3];     // in/out redirection
  struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
  int i = 0;
  printf("Command: <%s>\n", command->name);
  printf("\tIs Background: %s\n", command->background ? "yes" : "no");
  printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
  printf("\tRedirects:\n");
  for (i = 0; i < 3; i++)
    printf("\t\t%d: %s\n", i,
           command->redirects[i] ? command->redirects[i] : "N/A");
  printf("\tArguments (%d):\n", command->arg_count);
  for (i = 0; i < command->arg_count; ++i)
    printf("\t\tArg %d: %s\n", i, command->args[i]);
  if (command->next) {
    printf("\tPiped to:\n");
    print_command(command->next);
  }
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
  if (command->arg_count) {
    for (int i = 0; i < command->arg_count; ++i)
      free(command->args[i]);
    free(command->args);
  }
  for (int i = 0; i < 3; ++i)
    if (command->redirects[i])
      free(command->redirects[i]);
  if (command->next) {
    free_command(command->next);
    command->next = NULL;
  }
  free(command->name);
  free(command);
  return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
  char cwd[1024], hostname[1024];
  gethostname(hostname, sizeof(hostname));
  getcwd(cwd, sizeof(cwd));
  printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
  return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
  const char *splitters = " \t"; // split at whitespace
  int index, len;
  len = strlen(buf);
  while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
  {
    buf++;
    len--;
  }
  while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
    buf[--len] = 0; // trim right whitespace

  if (len > 0 && buf[len - 1] == '?') // auto-complete
    command->auto_complete = true;
  if (len > 0 && buf[len - 1] == '&') // background
    command->background = true;

  char *pch = strtok(buf, splitters);
  if (pch == NULL) {
    command->name = (char *)malloc(1);
    command->name[0] = 0;
  } else {
    command->name = (char *)malloc(strlen(pch) + 1);
    strcpy(command->name, pch);
  }

  command->args = (char **)malloc(sizeof(char *));

  int redirect_index;
  int arg_index = 0;
  char temp_buf[1024], *arg;
  while (1) {
    // tokenize input on splitters
    pch = strtok(NULL, splitters);
    if (!pch)
      break;
    arg = temp_buf;
    strcpy(arg, pch);
    len = strlen(arg);

    if (len == 0)
      continue; // empty arg, go for next
    while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
    {
      arg++;
      len--;
    }
    while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
      arg[--len] = 0; // trim right whitespace
    if (len == 0)
      continue; // empty arg, go for next

    // piping to another command
    if (strcmp(arg, "|") == 0) {
      struct command_t *c =
          (struct command_t *)malloc(sizeof(struct command_t));
      int l = strlen(pch);
      pch[l] = splitters[0]; // restore strtok termination
      index = 1;
      while (pch[index] == ' ' || pch[index] == '\t')
        index++; // skip whitespaces

      parse_command(pch + index, c);
      pch[l] = 0; // put back strtok termination
      command->next = c;
      continue;
    }

    // background process
    if (strcmp(arg, "&") == 0)
      continue; // handled before

    // handle input redirection
    redirect_index = -1;
    if (arg[0] == '<')
      redirect_index = 0;
    if (arg[0] == '>') {
      if (len > 1 && arg[1] == '>') {
        redirect_index = 2;
        arg++;
        len--;
      } else
        redirect_index = 1;
    }
    if (redirect_index != -1) {
      command->redirects[redirect_index] = (char *)malloc(len);
      strcpy(command->redirects[redirect_index], arg + 1);
      continue;
    }

    // normal arguments
    if (len > 2 &&
        ((arg[0] == '"' && arg[len - 1] == '"') ||
         (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
    {
      arg[--len] = 0;
      arg++;
    }
    command->args =
        (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
    command->args[arg_index] = (char *)malloc(len + 1);
    strcpy(command->args[arg_index++], arg);
  }
  command->arg_count = arg_index;

  // increase args size by 2
  command->args = (char **)realloc(command->args,
                                   sizeof(char *) * (command->arg_count += 2));

  // shift everything forward by 1
  for (int i = command->arg_count - 2; i > 0; --i)
    command->args[i] = command->args[i - 1];

  // set args[0] as a copy of name
  command->args[0] = strdup(command->name);
  // set args[arg_count-1] (last) to NULL
  command->args[command->arg_count - 1] = NULL;

  return 0;
}

void prompt_backspace() {
  putchar(8);   // go back 1
  putchar(' '); // write empty over
  putchar(8);   // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
  int index = 0;
  char c;
  char buf[4096];
  static char oldbuf[4096];

  // tcgetattr gets the parameters of the current terminal
  // STDIN_FILENO will tell tcgetattr that it should write the settings
  // of stdin to oldt
  static struct termios backup_termios, new_termios;
  tcgetattr(STDIN_FILENO, &backup_termios);
  new_termios = backup_termios;
  // ICANON normally takes care that one line at a time will be processed
  // that means it will return if it sees a "\n" or an EOF or an EOL
  new_termios.c_lflag &=
      ~(ICANON |
        ECHO); // Also disable automatic echo. We manually echo each char.
  // Those new settings will be set to STDIN
  // TCSANOW tells tcsetattr to change attributes immediately.
  tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

  show_prompt();
  buf[0] = 0;
  while (1) {
    c = getchar();
    // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

    if (c == 9) // handle tab
    {
      buf[index++] = '?'; // autocomplete
      break;
    }

    if (c == 127) // handle backspace
    {
      if (index > 0) {
        prompt_backspace();
        index--;
      }
      continue;
    }

    if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
      continue;
    }

    if (c == 65) // up arrow
    {
      while (index > 0) {
        prompt_backspace();
        index--;
      }

      char tmpbuf[4096];
      printf("%s", oldbuf);
      strcpy(tmpbuf, buf);
      strcpy(buf, oldbuf);
      strcpy(oldbuf, tmpbuf);
      index += strlen(buf);
      continue;
    }

    putchar(c); // echo the character
    buf[index++] = c;
    if (index >= sizeof(buf) - 1)
      break;
    if (c == '\n') // enter key
      break;
    if (c == 4) // Ctrl+D
      return EXIT;
  }
  if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
    index--;
  buf[index++] = '\0'; // null terminate string

  strcpy(oldbuf, buf);

  parse_command(buf, command);

  // print_command(command); // DEBUG: uncomment for debugging

  // restore the old settings
  tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  return SUCCESS;
}

int process_command(struct command_t *command) {

  while (waitpid(-1, NULL, WNOHANG) > 0) {} // cleans up any previously terminated child processes to avoid zombie processes

  int r;
  if (strcmp(command->name, "") == 0)
    return SUCCESS;

  if (strcmp(command->name, "exit") == 0)
    return EXIT;

  if (strcmp(command->name, "cd") == 0) {
    if (command->arg_count > 0) {
      r = chdir(command->args[1]);
      if (r == -1)
        printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
     return SUCCESS;
    }
  }

	//cut command 3rd commit
    // 3rd Commit Note: I thought about using realloc/free because I dont know how many fields the user will input but I will asume that it is under 300 for now

 if (strcmp(command->name, "cut") == 0) { // to not use fork and execv, to treat it as a builtin command 

  char delimiter = '\t';
  char fields_buf_temp[1024];   
  int have_fields = 0;
  int fields[300];
  int fields_count = 0;


  for (int i = 1; i < command->arg_count - 1; i++) {
    if (command->args[i] == NULL) break;

    // parsing option -d :
    if ((strcmp(command->args[i], "-d") == 0 || strcmp(command->args[i], "--delimiter") == 0) && 
        (i + 1 < command->arg_count - 1) && command->args[i + 1] != NULL) {
      delimiter = command->args[i + 1][0];
      i++;
    } 
    
    // parsing options -f : Copying the numbers user typed into the buffer
    if ((strcmp(command->args[i], "-f") == 0 || strcmp(command->args[i], "--fields") == 0) &&
               (i + 1 < command->arg_count - 1) && command->args[i + 1] != NULL) {
      
      snprintf(fields_buf_temp, sizeof(fields_buf_temp), "%s", command->args[i + 1]);
      have_fields = 1;
      i++;
    }
  }

  // if there is no -f have_fields stays 0 logically
  if (have_fields == 0) {
    return SUCCESS;
  } 

  char *saveptr = NULL;
  char *output = strtok_r(fields_buf_temp, ",", &saveptr); // To create the output with commas in between the number and appointing a pointer to the output
  while (output != NULL) {
  while (*output == ' ' || *output == '\t') output++; // To skip white spaces if there are any

  //3rd commit note: I did not use atoi() because I wanted to check whether the character is an integer or not  

  int val = 0; // the integer value
  int valid = 1; //is it a valid number check: for edge cases 
  for (char *p = output; *p; p++) {
    if (!isdigit((unsigned char)*p)) { valid = 0; break; }
    val = val * 10 + (*p - '0'); // ASCII numbers 
  }

  if (valid && val > 0 && val <= 300 && fields_count < 300) {
    fields[fields_count++] = val;      
  }

  output = strtok_r(NULL, ",", &saveptr);
}
  if (fields_count == 0) {
    return SUCCESS;
  }

  
  char line[4096];
  while (fgets(line, sizeof(line), stdin) != NULL) { // read the input with fgets, I used this because it is more secure than gets
    int len = (int)strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
      line[len - 1] = '\0';
      len--;
    }

    int start[301];
    int end[301];
    for (int i = 0; i <= 300; i++) {   
      start[i] = -1;
      end[i] = -1;
    }

    int field = 1;
    int s = 0;
    for (int i = 0; i <= len; i++) {
      if (i == len || line[i] == delimiter) {
        if (field <= 300) {           
          start[field] = s;
          end[field] = i; 
        }
        field++;
        s = i + 1;
        if (field > 300) break;       
      }
    }

  
    for (int k = 0; k < fields_count; k++) {
      if (k > 0) putchar(delimiter);

      int fidx = fields[k];
      if (fidx >= 1 && fidx <= 300 && start[fidx] != -1 && end[fidx] != -1) { 
        int a = start[fidx];
        int b = end[fidx];
        if (a >= 0 && b >= a && b <= len) {
          fwrite(line + a, 1, (size_t)(b - a), stdout);
        }
      }
    }
    putchar('\n');
  }

  return SUCCESS;
}


// timer command (foreground countdown)
// references https://d-libro.com/topic/foreground-and-background-jobs/

if (strcmp(command->name, "timer") == 0) {

    if (command->arg_count < 2 || command->args[1] == NULL) {
        printf("Usage: timer <number>s | <number>m | <number>h\n");
        return SUCCESS;
    }

    char argbuf[64];
    strncpy(argbuf, command->args[1], sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    int len = (int)strlen(argbuf);
    if (len < 2) {
        printf("Usage: timer <number>s | <number>m | <number>h\n");
        return SUCCESS;
    }

    char unit = argbuf[len - 1];
    if (unit != 's' && unit != 'm' && unit != 'h') {
        printf("Invalid unit '%c'. Use s, m, or h.\n", unit);
        printf("Usage: timer <number>s | <number>m | <number>h\n");
        return SUCCESS;
    }

    argbuf[len - 1] = '\0'; // strip unit, leave only digits

    // Validate: must be all digits
    for (int i = 0; argbuf[i] != '\0'; i++) {
        if (argbuf[i] < '0' || argbuf[i] > '9') {
            printf("Invalid time value. Use digits followed by s/m/h (e.g., 10s, 5m, 1h).\n");
            return SUCCESS;
        }
    }

    int value = atoi(argbuf);
    if (value <= 0) {
        printf("Please enter a positive number (e.g., 10s, 5m, 1h).\n");
        return SUCCESS;
    }

    long total_seconds_long = 0;
    if (unit == 's') total_seconds_long = (long)value;
    if (unit == 'm') total_seconds_long = (long)value * 60L;
    if (unit == 'h') total_seconds_long = (long)value * 3600L;

    // avoid overflow into int
    if (total_seconds_long > 24L * 3600L * 365L) { // optional sanity limit: 1 year
        printf("Timer value too large.\n");
        return SUCCESS;
    }

    int total_seconds = (int)total_seconds_long;

    // Install temporary SIGINT handler (Ctrl+C) just for the timer
    struct sigaction old_sa, sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = timer_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    timer_cancelled = 0;
    sigaction(SIGINT, &sa, &old_sa);

    printf("⌛ Timer started (%d%c) (Ctrl+C to cancel) ⌛\n", value, unit);

    for (int left = total_seconds; left >= 0; left--) {

        if (timer_cancelled) {
            printf("\033[0m\n🛑 Timer cancelled.\n");
            sigaction(SIGINT, &old_sa, NULL);
            return SUCCESS;
        }


        

       int hh, mm, ss, total_minutes;

       hh = left / 3600;
       mm = (left % 3600) / 60;
       ss = left % 60;

       /* choose color */
       if (left <= 10) printf("\033[1;31m");
       else printf("\033[1;32m");

       /* display: HH:MM:SS if 1 hour or more, otherwise MM:SS */
      if (left >= 3600) {
         printf("\r%02d:%02d:%02d left... ", hh, mm, ss);
      } else {
      total_minutes = left / 60;
      printf("\r%02d:%02d left... ", total_minutes, ss);

}

printf("\033[0m");
fflush(stdout);

        sleep(1);
    }

    printf("\n⏰ Timer finished!\n");

    sigaction(SIGINT, &old_sa, NULL);
    return SUCCESS;
}

  // Chatroom command starts here
  //4th commit note: I used as fixed aray size for the messages but I might consider using a dynamic memory allocation 
  //reference for the mkfifo() : https://www.geeksforgeeks.org/cpp/named-pipe-fifo-example-c-program/
  
  if (strcmp(command->name, "chatroom") == 0) { //if the user typed in chatroom command


    if (command->arg_count < 4 || command->args[1] == NULL || command->args[2] == NULL) {
      printf("Usage: chatroom <roomname> <username>\n");
      return SUCCESS;
    } // Checks if it is a valid input for the command chatroom

    const char *roomname = command->args[1];
    const char *username = command->args[2];


    char roomdir[4096];
    snprintf(roomdir, sizeof(roomdir), "/tmp/chatroom-%s", roomname);

   
    if (mkdir(roomdir, 0777) == -1) { //if the room exists/it cannot be created 
       // 0777: everytone can read write and execute the chatroom  (execute as in enter the chat)
      if (errno != EEXIST) {  //if the error stems from something other than the file having been already created before 
        //just prints the error and flags the process as a success, no longer continues
        printf("-%s: chatroom: mkdir: %s\n", sysname, strerror(errno));
        return SUCCESS;
      }
    }  // creates room directory if it doesnt exist

    
    char myfifo[4096];
    snprintf(myfifo, sizeof(myfifo), "%s/%s", roomdir, username);

    if (mkfifo(myfifo, 0666) == -1) {
    //0666: Eveyone can read and write but not execute, 
      if (errno != EEXIST) {
        printf("-%s: chatroom: mkfifo: %s\n", sysname, strerror(errno));
        return SUCCESS;
      }
  } //creates user's name pipe with mkfifo 

  //5t commmit will start here

  //Note to self: Do not forget to add #include <limits.h>

    printf("Welcome to %s!\n", roomname); //welcome message printed

    
    pid_t reciever_pid_child = fork(); // fork here to create a process (duplicating)

    // error check
    if  (reciever_pid_child == -1) {
      printf("-%s: chatroom: fork: %s\n", sysname, strerror(errno));
      return SUCCESS;
    } 

    // Two processes one for wiritng and one for sending the message and one for 
    //recieving the message 


    if  (reciever_pid_child == 0) { // detecting if child process 
    do {

        int fd = open(myfifo, O_RDWR);
        if (fd < 0) _exit(99);
        
        char rbuf[BUF_SIZE];

        ssize_t n = read(fd, rbuf, sizeof(rbuf));
        
        while (n > 0) { //if there is a message 

        write(STDOUT_FILENO, rbuf, n);  

        n = read(fd, rbuf, sizeof(rbuf));

        }
        close(fd);

      } while (1); //infinite loop 

      _exit(0);
    }


    // start here fot the 6th commit 
  
   char msg[MSG_SIZE];

    while (true) {
      printf("[%s] %s > <write your message here>\n", roomname, username);
      
      fflush(stdout); // ensures the prompt appears before reading input
    
      // https://www.quora.com/What-does-fflush-stdout-do-in-C-and-how-do-you-use-it reference for fflush

      if (fgets(msg, sizeof(msg), stdin) == NULL) { // if the user does not enter anything
      
       break;
      }

      msg[strcspn(msg, "\n")] = '\0'; // Since fgets puts a new line character once the user presses enter

      if (strcmp(msg, "/exit") == 0 || strcmp(msg, "exit") == 0) break;
      
   
      char out[BUF_SIZE];

      snprintf(out, sizeof(out), "[%s] %s: %s\n", roomname, username, msg);

      DIR *d = opendir(roomdir);

      if (!d) {
            printf("-%s: chatroom: opendir: %s\n", sysname, strerror(errno));
            continue;
            }

      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        if (strcmp(de->d_name, username) == 0) continue;

        char otherfifo[1024];
        snprintf(otherfifo, sizeof(otherfifo), "%s/%s", roomdir, de->d_name);

   
        pid_t spid = fork();
        if (spid < 0) { 
          printf("-%s: chatroom: fork: %s\n", sysname, strerror(errno));
          continue; 
        } 
        if (spid == 0) {


        int wfd = open(otherfifo, O_WRONLY);

        if (wfd < 0) {
             _exit(0);
        }
        (void)write(wfd, out, strlen(out));
        close(wfd);
        _exit(0);

        }
      }

      closedir(d);

      while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }

   
    kill(reciever_pid_child, SIGTERM); //kills the infinete loop above
    waitpid(reciever_pid_child, NULL, 0); 

    return SUCCESS;
  }

  pid_t pid = fork();
  if (pid == 0) // child
  {
    /// This shows how to do exec with environ (but is not available on MacOs)
    // extern char** environ; // environment variables
    // execvpe(command->name, command->args, environ); // exec+args+path+environ

    /// This shows how to do exec with auto-path resolve
    // add a NULL argument to the end of args, and the name to the beginning
    // as required by exec
    

    // 2nd commit note:  I might add a dup2 fail safety check in the third commit

    if (command->redirects[0]) {
      int fd_in = open(command->redirects[0], O_RDONLY);
      if (fd_in < 0) { 
	perror("open input"); 
	exit(1); }

      dup2(fd_in, STDIN_FILENO);
      close(fd_in);
    }

    if (command->redirects[1]) { // Cheking if the user typed something like ls > filename
      int fd_out = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, 0644); 
      if (fd_out < 0) { 
	perror("open output"); 
	exit(1); }
      dup2(fd_out, STDOUT_FILENO);
      close(fd_out);
    } // Anything in stdout printed, goes into the file

    if (command->redirects[2]) { // Checking if the user typed something like ls >> filename  
      int fd_app = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd_app < 0) { 
	perror("open append"); 
	exit(1); }
      dup2(fd_app, STDOUT_FILENO);
      close(fd_app);
    } // Output gets added to the end so no overwrite

    // 2nd commit note: Not sure about the pipe logic I might change it in the 3rd commit
    if (command->next) { //Checking if a pipe is used
      int pipefd[2]; //To create a pipe
      pipe(pipefd);
      pid_t pid2 = fork();

      if (pid2 == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[1]);
        close(pipefd[0]);
        process_command(command->next);
        exit(0);

      }
      else {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
      }
    }

    // TODO: do your own exec with path resolving using execv()
    // do so by replacing the execvp call below
    char *path_env = getenv("PATH");
    if (!path_env) { printf("command not found\n"); exit(127); } // To assure that the path exists (commit 2 fix)
    char *path_copy = strdup(path_env);
    char *dir = strtok(path_copy, ":");
    char full_path[1024];

    int test  = 0;

    while (dir != NULL) {
      snprintf(full_path, sizeof(full_path), "%s/%s", dir, command->name);
      if (access(full_path, X_OK) == 0) {
        test = 1; break;
      }
      dir = strtok(NULL, ":");
    }
  
   free(path_copy);
 
   if (test == 1) execv(full_path, command->args);

    printf("command not found\n");
    exit(127);

  } else {
    // TODO: implement background processes here

    if (command->background) printf("[Background] PID: %d\n", pid);  
 
    else waitpid(pid, NULL, 0); //insead of wait(0) to not wait for any child but specific ones (second commit fix)

    return SUCCESS;
  }
}


int main() {
  while (1) {
    struct command_t *command =
        (struct command_t *)malloc(sizeof(struct command_t));
    memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

    int code;
    code = prompt(command);
    if (code == EXIT)
      break;

    code = process_command(command);
    if (code == EXIT)
      break;

    free_command(command);
  }

  printf("\n");
  return 0;
}
