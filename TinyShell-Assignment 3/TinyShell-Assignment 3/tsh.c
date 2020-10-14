/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
      switch (c) {
        case 'h':             /* print help message */
          usage();
	        break;

        case 'v':             /* emit additional diagnostic info */
          verbose = 1;
	        break;

        case 'p':             /* don't print a prompt */
          emit_prompt = 0;  /* handy for automatic testing */
	        break;

	      default:
          usage();
	    }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {
	    /* Read command line */
	    if (emit_prompt) {
	      printf("%s", prompt);
	      fflush(stdout);
	    }

      if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	      app_error("fgets error");

      if (feof(stdin)) { /* End of file (ctrl-d) */
	      fflush(stdout);
	      exit(0);
	    }

	    /* Evaluate the command line */
	    eval(cmdline);
	    fflush(stdout);
	    fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
 
void getLastToken(char * source, char ** target) {
  int length; 
  char string[256]; 
  
  strcpy(string, source);

  length = strlen(string); 

  // Allocate a chunk of memory. 
  *target = (char *)malloc(length);

  assert(*target != NULL);

  char delim[] = "/";
  char *token = strtok(&string[1], delim);
  while(token != NULL ) {
      strcpy(*target, token);
     // fprintf(stderr, "token is %s\n", token);
      token = strtok(NULL, delim);
  }
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char buf[MAXLINE];   /* Holds modified command line */
    char *argv[MAXARGS]; /* Argument list execve() */

    int isBgJob;         /* Should the job run in bg or fg? */
    pid_t pid;           /* Process id */

    sigset_t mask, prev_mask;

    strcpy(buf, cmdline);
    isBgJob = parseline(buf, argv);
    if (argv[0] == NULL)
        return;   /* Ignore empty lines */

    //Initialize Signal Mask for each process
    sigemptyset(&mask);
  	sigaddset(&mask,SIGCHLD);
  	sigaddset(&mask,SIGINT);
  	sigaddset(&mask,SIGTSTP);

    // Check whether the user executes a built-in command (quit, jobs, bg, fg)
    if(builtin_cmd(argv) == 0) {
      // Block any signal before child is created
      sigprocmask(SIG_BLOCK, &mask, &prev_mask);

      //fprintf(stderr, "Command is not a built-in command\n");
      // Now the command is not a built-in command
      int status; 
      pid = fork(); 
      if(pid == 0) { // child process
        // Now the process is ready for signals.
        sigprocmask(SIG_SETMASK, &prev_mask, NULL); // Unblock sigMask
        
        // Set process group ID for job control
          int pgidIsSet = setpgid(0, 0);
          
          if (pgidIsSet < 0) {
              unix_error("setpgid error"); // Call unix error
          }

        // Execult this command
          int didExecve = execve(argv[0], argv, environ);
          
          if (didExecve < 0) { // Run Execve
              fprintf(stdout, "%s: ", argv[0]);
              app_error("This command was not found");
          }
      }
      else if (pid < 0) {
        unix_error("Can't run the command");        
      }
      else {
        // Handle the background job and forgreound job differently
          if (isBgJob) {
              // We want to add job, unblock thread, and print the result
              addjob(jobs, pid, BG, cmdline);
              sigprocmask(SIG_UNBLOCK, &mask, NULL);
              
              // Handle the signals
              printf("[%d](%d)%s", pid2jid(pid), pid, buf);
              
          } else {
              // We want to add job, unblock thread, and then wait
              addjob(jobs, pid, FG, cmdline);
              sigprocmask(SIG_UNBLOCK, &mask, NULL);
              
              // Handle the signals
              waitfg(pid);
              
          }
      }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	    buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
    }
    else {
	    delim = strchr(buf, ' ');
    }

    while (delim) {
	    argv[argc++] = buf;
	    *delim = '\0';
	    buf = delim + 1;
	    while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	    if (*buf == '\'') {
	      buf++;
	      delim = strchr(buf, '\'');
	    }
	    else {
	      delim = strchr(buf, ' ');
	    }
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	    return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	    argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 *  1. The quit command terminates the shell.
 *  2. The jobs command lists all background jobs.
 *  3. The bg <job> command restarts <job> by sending it a SIGCONT signal, and then runs it in the background. The <job> argument can either be a PID or a JID.
 *  4. The fg <job> command restarts <job> by sending it a SIGCONT signal, and then runs it in the foreground.  The <job> argument can be either a PID or a JID.
 */
int builtin_cmd(char **argv) 
{
    assert(argv != NULL);
   
    if(strcmp(argv[0], "quit") == 0) {
      fprintf(stderr, "Quit the shell now!\n");
      exit(0);
      return 1;
    }
    else if(strcmp(argv[0], "jobs") == 0) {
      // List all background jobs
 //     fprintf(stderr, "Run the built-in %s command!\n", argv[0]);
      listjobs(jobs);
      return 1; 
    }
    else if((strcmp(argv[0], "bg") == 0) || (strcmp(argv[0], "fg") == 0) ) {
   //   fprintf(stderr, "do_bgfg\n");
      do_bgfg(argv);
      return 1; 
    }
    else {
     // fprintf(stderr, "we are in the else branch\n");
      return 0;     /* not a builtin command */
    }
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 *  It should identify the process PID with the specified job id or process id befoore sending a SIGCONT to the specified process. In the end, it should handle the background and foreground process differently.  
 */
void do_bgfg(char **argv) 
{
    // MARK: Need to run a series of checks in order to differentiate job and determine its legitimacy
    
    struct job_t *incomingJob; // Variable to store incoming job
    
    // Check if multiple arguments are present
    if (argv[1] == NULL || argv[2] != NULL) {
        printf("%s: command requires PID or %%jobid argument\n", argv[0]);
        return;
    } else {
        
        // Check if the second argument is a PID or JOBID or neither
        if (argv[1][0] != '%' && !isdigit(argv[1][0])) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return ;
        } else {
            
            // Determine if jobID or PID
            if (argv[1][0] == '%') {
                
                // The task is a jobID treat accordingly:
                incomingJob = getjobjid(jobs, atoi(&argv[1][1])); // Set the job to the argument value
                
                // Check to see if the job is NULL
                if (incomingJob == NULL) {
                    printf("%s: No such job\n", argv[1]);
                    return;
                }
            } else {
                
                // We know that the task is a process, treat accordingly:
                incomingJob = getjobjid(jobs, (pid_t) atoi(argv[1]));
                
                if (incomingJob == NULL) {
                    printf("(%d): No such process \n", atoi(argv[1]));
                    return;
                }
                
            }
            
            // MARK: Determine whether or not the process is running in the background or foreground
            
            if (strcmp(argv[0], "fg") == 0) {
                
                // This is a foregroudn process: Run Accordingly
                incomingJob->state = FG;
                kill(-incomingJob->pid, SIGCONT);
                waitfg(incomingJob->pid);
                
            } else {
                
                // This is a background process; Run Accordingly:
                incomingJob->state = BG;
                printf("[%d] (%d) %s", incomingJob->jid, incomingJob->pid, incomingJob->cmdline);
                kill(-incomingJob->pid, SIGCONT);
            }
        }
    }
    
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    // Need to wait until the process has successfully completed or is no longer in the foreground
    
    // Variables to track the mask and previous masks of the job
    sigset_t previousMask;
    sigset_t mask;
    
    // Setup a SIG BLOCK on mask and previous mask
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &previousMask);
    
    // Wait until the pid is no longer in the list of jobs
    while (1) {
        if (pid != fgpid(jobs)) {
            printf("Process (%d) is no longer in the foregroudn \n", (int) pid);
            break;
        } else {
            sigsuspend(&previousMask);
        }
    }
    
    // Cleanup and return back
    sigprocmask(SIG_SETMASK, &previousMask, NULL);
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    // Put your code here. 
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
  // Put your code here. 
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  fprintf(stderr, "Change the state of the job\n");
  return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].jid > max)
	      max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	    return 0;

    for (i = 0; i < MAXJOBS; i++) {
	    if (jobs[i].pid == 0) {
	      jobs[i].pid = pid;
	      jobs[i].state = state;
	      jobs[i].jid = nextjid++;
	      if (nextjid > MAXJOBS)
		      nextjid = 1;
	    
        strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
        }
        return 1;
	    }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	    return 0;

    for (i = 0; i < MAXJOBS; i++) {
	    if (jobs[i].pid == pid) {
	      clearjob(&jobs[i]);
	      nextjid = maxjid(jobs)+1;
	      return 1;
	    }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].state == FG)
	      return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	    return NULL;
    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].pid == pid)
	      return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	    return NULL;
    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].jid == jid)
	      return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	    return 0;
    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].pid == pid) {
         return jobs[i].jid;
      }
    
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	    if (jobs[i].pid != 0) {
	      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    
        switch (jobs[i].state) {
		      case BG: 
		        printf("Running background ");
		        break;
		
          case FG: 
		        printf("Foreground ");
		        break;

		      case ST: 
		        printf("Stopped ");
		        break;

	        default:
		        printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
	      } 

	      printf("%s\n", jobs[i].cmdline);
	    }
  }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	    unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



