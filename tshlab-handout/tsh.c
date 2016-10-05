/* 
 * tsh - A tiny shell program with job control
 *
 * 학번: 201201356
 * 이름: 김민호 
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */
/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */


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
struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

extern char **environ;      /* defined in libc */
char prompt[] = "eslab_tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void waitfg(pid_t pid, int output_fd);
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
void listjobs(struct job_t *jobs, int output_fd);

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
	Signal(SIGTTIN, SIG_IGN);
	Signal(SIGTTOU, SIG_IGN);

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
		if (feof(stdin)) { /* End of file (ctrl-d) */	// ctrl + d 를 입력하면 종료 
			fflush(stdout);
			fflush(stderr);
			exit(0);
		}

		/* Evaluate the command line */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	} 

	exit(0); /* control never reaches here */
}
// main() 함수는 쉘이 실행되면 먼저 main() 함수에서 한 줄의 명령어 command line을 입력받아 저장한 후 
// eval() 함수를 수행하고 eval() 함수에서는 입력받은 command line에 대해 처리를 하는 역할을 한다. 
// 그 전에 main() 함수에서는 signal들을 정의하고 
// job list를 initjob() 함수를 통해 초기화 하는 작업도 같이 하고 있고, 
// ctrl+d와 같은 인터럽트를 통해 shell을 종료할 수 있도록 되어 있다. 
// main()에서 중요한 부분은 사용자가 명령어를 입력하면 main() 에서는 eval() 함수를 호출하여 
// 입력한 명령어에 대해서 처리를 하게 하는 것이다. 즉 shell이 종료될 때까지 
// 이 과정을 무한 반복하는 것이다.


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
	char *argv[MAXARGS]; // command
	pid_t pid;	// process ID 
	int bg;	// BG, FG check 
	sigset_t mask;
	
	bg = parseline(cmdline, argv); // 명령어를 argv에 분류하여 BG, FG 체크 
	
	if (!builtin_cmd(argv)) {
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGTSTP);
	
		if ( sigprocmask( SIG_BLOCK, &mask, NULL ) < 0 )	// SIG_BLOCK 에러처리 
			unix_error("error: SIG_BLOCK");
		
		// 시그널 마스킹에 쓰일 mask변수를 초기화 후 SIGCHLD SIGINT SIGTSTP
		// 시그널을 BLOCK 시켜 일시적으로 현재 tsh에 전송되는 시그널을 차단하여. 
		// RACE CONDITION이 발생하지 않도록 한다. 
		// Race Condition 발생은 addjobs()함수를 호출하기 전에 시그널이
		// 발생되었을 경우 Jobs에 해당 프로세스가 등록되어있지 않으므로 에러가 발생함 
	
			
		if((pid=fork()) == 0) {	// fork로 자식프로세스 생성
		
			setpgid(0, 0);	// 프로세스를 프로세스 그룹 ID로 포함한다. 
		
			if ( sigprocmask( SIG_UNBLOCK, &mask, NULL ) < 0 )	
			// SIG_UNBLOCK 에러처리 
			
				unix_error("error: SIG_UNBLOCK");
			//새로운 자식 프로세스가 시그널을 입력받을 수 있도록 UNBLOCK 한다. 
		
			if((execve(argv[0], argv, environ) < 0)) {	// 2번째 인자는 매개변수 
				printf("%s: Command not found\n", argv[0]);
				exit(0);
			}
			// 자식 프로세스가 수행할 프로그램을 execve를 사용하여 점프
			// 에러가 나면 command not found, exit(0)으로 예외처리 
		}
		
		if (!bg) {	// foreground job
			addjob(jobs, pid, FG, cmdline);	// foreground job을 job list에 추가 
			
			if ( sigprocmask( SIG_UNBLOCK, &mask, NULL ) < 0 )
				unix_error("error: SIG_UNBLOCK");
			//이제 부모 프로세스도 시그널을 처리 할수 있도록 UNBLOCK 한다. 
			
			waitfg(pid, 1);	// 모든 자식 프로세스가 종료될 때까지 기다린다. 
		} else {	// background job
			addjob(jobs, pid, BG, cmdline);	// background job을 job list에 추가 
		
			if ( sigprocmask( SIG_UNBLOCK, &mask, NULL ) < 0 )
				unix_error("error: SIG_UNBLOCK");
			//이제 부모 프로세스도 시그널을 처리 할수 있도록 UNBLOCK 한다. 
		
			printf("(%d) (%d) %s", pid2jid(pid), pid, cmdline);	// background 정보 출력 
		}
	}	
	return;
}
// eval() 함수는 parseline() 함수를 호출하여 입력받은 commad line을 각각 나누어 argv변수에 저장한다. 
// 입력받은 명령이 jobs, fg, bg, quit과 같은 명령어라면 
// 그에 대응하는 명령을 builtin_cmd() 함수를 통해 수행하게 하고, 
// 그렇지 않으면 fork() 함수로부터 자식 프로세스를 만들어 
// 자식 프로세스를 명령어에 대한 프로그램을 수행 시키는 execve() 함수를 이용하는 과정이다. 
// 만약 자식 프로세스가 성공적으로 프로그램을 수행 시키면 
// job list에 foreground, background job을 구분하여 addjob()을 통해 job을 추가하는 역할을 한다.
// eval() 함수는 사용자가 입력한 명령어에 대해서 처리를 하는 함수라고 볼 수 있다.

int builtin_cmd(char **argv)
{
	char *cmd = argv[0];	
	int flag, jid;
	pid_t pid;
	struct job_t *job;	

	if(!strcmp(cmd, "quit")) {	// quit 명령어를 입력하면 종료한다. 
		exit(0);
	}
	else if(!strcmp(cmd, "jobs")) {	// jobs 명령어를 입력하면 joblist를 출력한다.
		listjobs(jobs, STDOUT_FILENO);
		return 1;
	}	
	else if(!strcmp(cmd, "bg") || !strcmp(cmd, "fg")) {	// bg, fg 명령어 입력 처리 
	
		if( !strcmp(cmd, "fg") )	// fg, bg를 체크하여 flag에 저장 
			flag = FG;
		else
			flag = BG;

		if( argv[1][0] == '%' ){	// %를 확인하여 그 이후 job id를 찾는다. 
			job = getjobjid(jobs,atoi(&argv[1][1]));	
			// atoid() 함수 사용하여 % 이후의 job id 문자열을 숫자로 바꾸고
			// getjobjid() 함수를 이용하여 해당 job id의 job을 가져온다. 
			
			if(job == NULL) {
				printf("%s: No Such Job\n", argv[1]);
				return 1;
			}
			// 가져온 job이 null이면 해당 job이 없는 경우 예외처리 
		}

		pid = job->pid;	// 가져온 job의 프로세스 id인 pid를 저장 
		jid = job->jid;	// 가져온 job의 job id인 jid를 저장 
			
		if (flag == BG) {	// BG 명령어를 입력했을 때 
			if(job->state == ST) {
				kill(-pid, SIGCONT);	// 중단된 프로세스를 다시 실행한다. 
				job->state = flag;	// 다시 실행된 job의 state를 BG로 바꿔준다. 
				printf("[%d] (%d) %s",jid,pid,job->cmdline);
			}
		}
		else if (flag == FG){ // FG 명령어를 입력했을 때 
			kill(-pid, SIGCONT);	
			// 중단된 프로세스를 SIGCONT signal을 보내어 다시 실행 
			job->state = flag;	
			// 다시 실행된 job의 state를 FG로 바꾸어 foreground에서 실행 
			waitfg(pid, 1);	// 모든 자식이 종료됨을 기다린다. 
		}
		return 1;
	}
	return 0;
}

void waitfg(pid_t pid, int output_fd)
{
	struct job_t *j = getjobpid(jobs, pid);
	char buf[MAXLINE];
	
	if(!j)	// foreground job이 아닐경우 return 
		return;

	while (j->pid == pid && j->state ==FG)
		sleep(1);
	// 프로세스가 존재하고 FG일 경우
	// 실행되고 있는 foreground job이 종료될 때까지 기다린다. 
	
	if(verbose) {
		memset(buf, '\0', MAXLINE);
		sprintf(buf, "waitfg: Process (%d) no longer the fg process:q\n", pid);
		if(write(output_fd, buf, strlen(buf)) < 0) {
			fprintf(stderr, "Error writing to file\n");
			exit(1);
		}
	}
	return;
}
// foreground job을 위해 프로세스를 일시적으로 중단하는 함수이다. 
// foreground Job은 종료될 때 까지 계속 사용자의 포커스를 유지하므로 
// foreground Job이 종료 될 때까지 eval()함수가 리턴해 
// 새로운 작업을 입력받지 못하도록 대기 상태를 유지 시켜주는 함수이다.
// 넘겨받은 pid값으로 해당 프로세스가 아직 종료되지 않았고 
// Background job으로 전환되지 않았는지의 여부를 계속해서 검사하며
// sleep() 함수를 호출하여 기다리게 한다.


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

	int status;
	pid_t child_pid;

	// 자식 프로세스가 종료 혹은 중단된 상태를 처리한다. 
	while((child_pid = waitpid(-1 ,&status, WNOHANG|WUNTRACED)) > 0){ 
	// 자식프로세스가 모두 종료됨을 기다린다 
	
		if((WIFEXITED(status))>0){
			if(!(deletejob(jobs,child_pid)))	// delete job 에러처리 
				printf("error: delete job\n");
		}
		else if((WIFSIGNALED(status))!=0){	// 프로세스 종료 
			printf("Job [%d] (%d) terminated by signal %d\n",pid2jid(child_pid),child_pid, WTERMSIG(status));
			// SIGINT 2번, SIGTERM 15번 처리
						
			if(!(deletejob(jobs,child_pid)))	// job list에서 종료된 프로세스의 job을 삭제 
				printf("error: delete job\n"); 
		}
		else if((WIFSTOPPED(status))==1){	// 프로세스 중단 
			struct job_t* j=getjobpid(jobs,child_pid);
			j->state=ST;	// state를 ST상태로 바꾼다. 
			printf("Job [%d] (%d) stopped by signal %d\n",pid2jid(child_pid),child_pid, WSTOPSIG(status));
			// SIGTST 20번 처리 
		}
	}
	return;
}
// 자식 프로세스가 종료되거나 중단되면 커널이 부모 프로세스에게 SIGCHLD 시그널을 전송한다. 
// 함수 내부에서는 종료 혹은 중단된 프로세스를 waitpid()함수를 사용하여 검색하고
// 어느 프로세스가 종료되었으면 job list에서 삭제를하고 
// 어느 프로세스가 중단되었으면 jobs에서 state를 ST로 변경하여 중단된 상태를 알려주는 작업을 처리한다.
// while()을 통해 종료 되거나 중단된 프로세스가 더 이상 없을 때 까지 반복한다. 
// WNOHANG|WUNTRACED는 모든 자식 프로세스들이 정지하였거나 종료하였다면 리턴값을 0으로 즉시 리턴하거나
// 					자식들 중 한개의 pid와 동일한 값으로 리턴한다.
// WIFEXITED는 자식 프로세스가  정상적으로 종료되었다면 true를 리턴한다.
// WIFSIGNALED는 시그널을 받지 못해서 자식프로세스가 종료되었다고 판단할 때 true를 리턴한다. 
// WIFSTOPPED는 자식 프로세스가 현재 정지된 상태라면 true를 리턴한다. 

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	pid_t pid = fgpid(jobs);	// foreground job의 pid만 처리한다 
	if (pid != 0)
		kill(-pid, 2);	// 모든 foreground job을 종료, SIGINT (2)
	return;
}
// ctrl + c (SIGINT) 입력인 키보드 인터럽트가 발생되면 
// singnal이 발생되어 shell로 전달되는데 모든 foreground job들을 kill을 해줘야 한다.
// sigint_handler()에서는 kill() 함수를 통해 signal을 shell에 전달한다. 
// 이 때 모든 foreground job을 kill 해줘야 하므로 -pid의 값을 넘겨준다. 
// pid 값이 0보다 작으면 pid에 해당하는 그룹의 프로세스에게 signal을 보내므로 한번에 kill 할 수 있다.
// SIGINT에 해당하는 정수값은 2이므로 ctrl+c에 대한 signal을 shell에 보내어 처리할 수 있도록 한다.
// fgpid() 함수는 state가 FG인 job의 pid를 반환하는 함수이다.
// 이 함수를 이용하여 foreground job의 pid만 처리하도록 한다. 


/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
	pid_t pid = fgpid(jobs);	// fgpid()는 state가 FG인 job의 pid를 반환한다 
	if (pid != 0)
		kill(-pid, 20);	// 모든 foreground job을 STOP, SIGTSTP (20) 
	return;
}
// ctrl + z (SIGTST) 입력인 키보드 인터럽트가 발생되면
// shell에 signal을 보내어 처리할 수 있도록 하는데 모든 foreground job들을 kill을 한다
// 모든 foreground job에 대하여 STOP이 되어야 하므로 -pid 값을 넘겨주어 kill()을 수행한다.
// SIGTST에 해당하는 정수값은 20이고, ctrl+z에 대한 signal을 shell에 넘겨 처리할 수 있도록 한다.
// fgpid() 함수를 사용하여 foreground job의 프로세스만 처리할 수 있도록 한다. 

/*********************
 * End signal handlers
 *********************/


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
	buf[strlen(buf)-1] = ' '; /* replace trailing '\n' with space */
	while(*buf && (*buf == ' '))
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
	if ((bg = (*argv[argc-1] == '&')) != 0)
		argv[--argc] = NULL;

	return bg;

}

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
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
				printf("Added job. [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
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
void listjobs(struct job_t *jobs, int output_fd) 
{
	int i;
	char buf[MAXLINE];

	for (i = 0; i < MAXJOBS; i++) {
		memset(buf, '\0', MAXLINE);
		if (jobs[i].pid != 0) {
			sprintf(buf, "(%d) (%d) ", jobs[i].jid, jobs[i].pid);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			switch (jobs[i].state) {
				case BG:
					sprintf(buf, "Running ");
					break;
				case FG:
					sprintf(buf, "Foreground ");
					break;
				case ST:
					sprintf(buf, "Stopped ");
					break;
				default:
					sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
							i, jobs[i].state);
			}
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			sprintf(buf, "%s", jobs[i].cmdline);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
		}
	}
	if(output_fd != STDOUT_FILENO)
		close(output_fd);
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
	printf("Usage; shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information \n");
	printf("   -p   do not emit a command prompt \n");
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

