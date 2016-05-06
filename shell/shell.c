/*
 * Author  : Jakub Šoustar <jakub.soustar@gmail.com> <xsoust02@stud.fit.vutbr.cz>
 * Project : Shell
 *
*/

#define _XOPEN_SOURCE 500

#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#define EFFECTIVE_BUFFER_SIZE 512
#define BUFFER_SIZE 513
#define ARGS_SIZE 4

/*
 * Represents a command entered by the user.
 *
*/
typedef struct {
	// True if the command should be interrupt in background.
	int run_in_bg;
	// NULL-terminated array of command's arguments.
	char **args;
	// Name of the file to redirect command's stdout to.
	char *out;
	// Name of the file to redirect command's stdin to.
	char *in;
} command_t;

static inline void command_clear(command_t *command) {
	free(command->args);

	command->run_in_bg = 0;
	// These just point to the 'buffer', no freeing needed.
	command->args = NULL;
	command->out = NULL;
	command->in = NULL;
}

/*
 * Represents a process executing user's command.
 *
*/
typedef struct process_t {
	// Simple linked list.
	struct process_t *next;
	// True if the process is still running.
	// This is set to false when SIGCHLD is handled.
	int running;
	// Pid of the process.
	pid_t pid;
} process_t;

static inline void process_free(process_t *process) {
	free(process);
}

static const char *CMD_EXIT = "exit";
static const char *PROMPT = "$ ";

static const char RUN_IN_BG = '&';
static const char REDIR_OUT = '>';
static const char REDIR_IN = '<';

static volatile sig_atomic_t interrupt = 0;
static volatile pid_t fg_pid = -1;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static process_t *bg_head = NULL;

static char buffer[BUFFER_SIZE];
static int new_command = 0;

/*
 * Display shell's prompt. If there are any terminated background
 * processes, notification for each one is printed before the
 * prompt itself. Notifications aren't printed in any particular order.
 *
*/
void prompt_show() {
	process_t *curr_process = bg_head;
	process_t *prev_process = NULL;
	process_t *old_process;

	// Print notifications about finished background processes.
	while (curr_process != NULL) {
		if (! curr_process->running) {
			printf("[%d] Finished\n", curr_process->pid);
			old_process = curr_process;

			// Remove the information from the list.
			if (prev_process == NULL) {
				bg_head = curr_process = curr_process->next;
			} else {
				prev_process->next = curr_process->next;
				curr_process = curr_process->next;
			}

			process_free(old_process);
			continue;
		}

		prev_process = curr_process;
		curr_process = curr_process->next;
	}

	printf("%s", PROMPT);
	fflush(stdout);
}

/*
 * Parse an user's command stored in the 'buffer'. Arguments
 * and other information is stored in the 'command'.
 * Parsing is done in-situ in the 'buffer'.
 * Returns 0 on success; -1 otherwise.
 *
 * WARNING: Modifies contents of the 'buffer'!
 *
*/
int command_parse(command_t *command) {
	size_t args_size = ARGS_SIZE;
	char token = '\0';
	int ignore = 0;
	int pos = 0;

	if ((command->args = malloc(args_size * sizeof(char *))) == NULL) {
		perror("malloc");
		return -1;
	}

	command->run_in_bg = 0;
	command->out = NULL;
	command->in = NULL;

	for (int i = 0; i < EFFECTIVE_BUFFER_SIZE; i++) {
		if (isspace(buffer[i]) || buffer[i] == '\0') {
			// Preemptive string termination.
			buffer[i] = '\0';
			ignore = 0;
			continue;
		}

		if (buffer[i] == RUN_IN_BG || buffer[i] == REDIR_OUT || buffer[i] == REDIR_IN) {
			if (buffer[i] == RUN_IN_BG) {
				command->run_in_bg = 1;
			}

			// Store the token and terminate previous argument.
			token = buffer[i];
			buffer[i] = '\0';
			ignore = 0;
			continue;
		}

		// Argument's beginning. Just store pointer to the buffer.
		if (! ignore) {
			if (token == REDIR_IN) {
				command->in = &buffer[i];
			} else if (token == REDIR_OUT) {
				command->out = &buffer[i];
			} else {
				command->args[pos++] = &buffer[i];
			}

			// Just read the rest of the argument.
			ignore = 1;
		}

		if (args_size <= pos) {
			args_size <<= 1;

			if ((command->args = realloc(command->args, args_size * sizeof(char *))) == NULL) {
				perror("realloc");
				return -1;
			}
		}
	}

	// NULL-terminate arguments.
	while (pos < args_size) {
		command->args[pos++] = NULL;
	}

	return 0;
}

/*
 * Redirect command's stdout to the requested file.
 * Exits on failure.
 *
*/
void command_redirect_out(command_t *command) {
	static mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	static int flags = O_WRONLY | O_TRUNC | O_CREAT;
	int fd;

	if (command->out != NULL) {
		if ((fd = open(command->out, flags, mode)) == -1) {
			fprintf(stderr, "Couldn't open file '%s'.\n", command->out);
			exit(EXIT_FAILURE);
		}

		if (dup2(fd, STDOUT_FILENO) == -1) {
			perror("dup2");
			close(fd);
			exit(EXIT_FAILURE);
		}

		close(fd);
	}
}

/*
 * Redirect command's stdin to the requested file.
 * Exits on failure.
 *
*/
void command_redirect_in(command_t *command) {
	int fd;

	if (command->in != NULL) {
		if ((fd = open(command->in, O_RDONLY)) == -1) {
			fprintf(stderr, "Couldn't open file '%s'.\n", command->in);
			exit(EXIT_FAILURE);
		}

		if (dup2(fd, STDIN_FILENO) == -1) {
			perror("dup2");
			close(fd);
			exit(EXIT_FAILURE);
		}

		close(fd);
	}
}

/*
 * Fork a new process for the command. Will wait for the process
 * to terminate if the command's 'run_in_bg' is set to false.
 * Returns 0 on success; -1 otherwise.
 *
*/
int command_fork(command_t *command) {
	pid_t c_pid, w_pid;
	int status;

	if ((c_pid = fork()) < 0) {
		perror("fork");
		return -1;
	}

	// Parent process
	if (c_pid > 0) {
		if (! command->run_in_bg) {
			// Foreground process.
			fg_pid = c_pid;

			// Wait for the foreground process to terminate.
			do {
				if ((w_pid = waitpid(c_pid, &status, 0)) == -1) {
					// Since SIGCHLD is handled by the main thread,
					// there is a race between this and the main thread
					// to receive the child's return code.
					//
					// If the signal handler is executed first, waitpid
					// fails with ECHILD which we'll ignore.
					//
					// Either way, we end up with terminated foreground
					// process and 'fg_pid' set to -1.
					if (errno != ECHILD) {
						perror("waitpid");
						fg_pid = -1;
						return -1;
					}

					break;
				}
			} while(! (WIFEXITED(status) || WIFSIGNALED(status)));

			fg_pid = -1;
		} else {
			// Background process.
			process_t *process;

			if ((process = malloc(sizeof(process_t))) == NULL) {
				perror("malloc");
				return -1;
			}

			printf("[%d] Started\n", (int) c_pid);

			process->next = NULL;
			process->pid = c_pid;
			process->running = 1;

			// Store the basic information about the running process.
			if (bg_head == NULL) {
				bg_head = process;
			} else {
				process->next = bg_head;
				bg_head = process;
			}
		}
	}

	// Child process.
	if (c_pid == 0) {
		sigset_t mask;

		// Redirect process' stdout and stdin, if requested by the command.
		command_redirect_out(command);
		command_redirect_in(command);
		sigemptyset(&mask);

		// Background commands ignore SIGINT sent to the foreground one.
		if (command->run_in_bg) {
			sigaddset(&mask, SIGINT);
		}

		// Unblock all signals blocked by the command handling thread.
		pthread_sigmask(SIG_SETMASK, &mask, NULL);

		// Will return only when error occurred.
		execvp(command->args[0], command->args);
		perror("execvp");
		exit(EXIT_FAILURE);
	}

	return 0;
}

/*
 * Handles built-in 'exit' command. Sends SIGKILL to any
 * running background process and frees the list used to
 * track them.
 *
*/
void command_exit_handler() {
	process_t *curr_process = bg_head;
	process_t *old_process;

	while (curr_process != NULL) {
		if (curr_process->running) {
			kill(curr_process->pid, SIGKILL);
		}

		old_process = curr_process;
		curr_process = old_process->next;
		old_process->next = NULL;

		free(old_process);
	}

	bg_head = NULL;
	// Stop the running threads.
	interrupt = 1;
}

/*
 * Executes a command stored in the 'buffer', if there is any.
 * Returns 0 on success; -1 otherwise.
 *
*/
int command_execute() {
	command_t command;

	// Try to parse a command.
	if (command_parse(&command) != 0) {
		command_clear(&command);
		return -1;
	}

	// Check if some command was actually parsed.
	// We need at least a name of the program to execute.
	if (command.args == NULL || command.args[0] == NULL) {
		command_clear(&command);
		return 0;
	}

	// Built-in exit command.
	if (strcmp(command.args[0], CMD_EXIT) == 0) {
		command_clear(&command);
		command_exit_handler();
		return 0;
	}

	command_fork(&command);
	command_clear(&command);

	return 0;
}

/*
 * Thread handling user's commands. Each command is parsed
 * and executed. Once a command is successfully executed,
 * input handling thread is signaled.
 *
*/
void *commands_handler() {
	while (! interrupt) {
		if (pthread_mutex_lock(&mutex) != 0) {
			interrupt = 1;
			perror("pthread_mutex_lock");
			exit(EXIT_FAILURE);
		}

		while (! new_command) {
			if (pthread_cond_wait(&cond, &mutex) != 0) {
				interrupt = 1;
				perror("pthread_cond_wait");
				exit(EXIT_FAILURE);
			}
		}

		command_execute();
		new_command = 0;

		// Signal the input handling thread.
		if (pthread_cond_signal(&cond) != 0) {
			interrupt = 1;
			perror("pthread_cond_signal");
			exit(EXIT_FAILURE);
		}

		if (pthread_mutex_unlock(&mutex) != 0) {
			interrupt = 1;
			perror("pthread_mutex_lock");
			exit(EXIT_FAILURE);
		}
	}

	pthread_exit(NULL);
}

/*
 * Read user's input and store it in the 'buffer'.
 * Returns 0 on success; -1 otherwise.
 *
*/
int input_read() {
	ssize_t num_bytes;

	// Clear previous command to prevent unexpected leaking.
	memset(buffer, '\0', sizeof(buffer));
	num_bytes = read(STDIN_FILENO, &buffer, BUFFER_SIZE);

	if (num_bytes < 0) {
		perror("read");
		return -1;
	}

	if (num_bytes > EFFECTIVE_BUFFER_SIZE) {
		fprintf(stderr, "Input is too long. Maximum length is %d\n", EFFECTIVE_BUFFER_SIZE);

		// Read rest of the line.
		if (buffer[num_bytes - 1] != '\n') {
			while (getchar() != '\n');
		}

		return -1;
	}

	if (num_bytes == 0) {
		// Handle EOF just as if the user entered 'exit' command.
		strcpy(buffer, CMD_EXIT);
		printf("%s\n", buffer);
	} else {
		if (buffer[num_bytes - 1] == '\n') {
			// Input was terminated with new line.
			// Terminate the string by replacing the new line symbol.
			buffer[num_bytes - 1] = '\0';
		} else {
			// Input was terminated by EOF.
			// Terminate the string by appending '\0' to the line.
			buffer[num_bytes] = '\0';
			printf("\n");
		}
	}

	return 0;
}

/*
 * Thread handling user's input. Input is stored in the 'buffer'.
 * Once a command is successfully read, command handling thread
 * is signaled.
 *
*/
void *input_handler() {
	while (! interrupt) {
		if (pthread_mutex_lock(&mutex) != 0) {
			interrupt = 1;
			perror("pthread_mutex_lock");
			exit(EXIT_FAILURE);
		}

		prompt_show();

		if (input_read() == 0) {
			new_command = 1;

			// Signal the command handling thread.
			if (pthread_cond_signal(&cond) != 0) {
				interrupt = 1;
				perror("pthread_cond_signal");
				exit(EXIT_FAILURE);
			}

			// Wait until the command is processed.
			while (new_command) {
				if (pthread_cond_wait(&cond, &mutex) != 0) {
					interrupt = 1;
					perror("pthread_cond_wait");
					exit(EXIT_FAILURE);
				}
			}
		}

		if (pthread_mutex_unlock(&mutex) != 0) {
			interrupt = 1;
			perror("pthread_mutex_lock");
			exit(EXIT_FAILURE);
		}
	}

	pthread_exit(NULL);
}

/*
 * Handler for SIGCHLD and SIGINT signals.
 *
*/
void sig_handler(int sig_num) {
	if (sig_num == SIGCHLD) {
		process_t *process;
		pid_t c_pid;

		while ((c_pid = waitpid(-1, NULL, WNOHANG)) > 0) {
			if (c_pid == fg_pid) {
				// No further action is required. See commands handler.
				fg_pid = -1;
			} else {
				process = bg_head;

				// Update process' state. Notification will be printed next
				// time the prompt is displayed.
				while (process != NULL) {
					if (process->pid == c_pid) {
						process->running = 0;
						break;
					}

					process = process->next;
				}
			}
		}
	}

	if (sig_num == SIGINT) {
		printf("\n");

		if (fg_pid != -1) {
			// Pass it the foreground process. Shell keeps running.
			kill(fg_pid, SIGINT);
		} else {
			// User is just playing with the keyboard.
			prompt_show();
		}
	}
}

int main() {
	struct sigaction sig_action;
	pthread_t commands_thread;
	pthread_t input_thread;
	sigset_t sig_mask;

	// Block all signals. This will be inherited by both handler threads.
	sigfillset(&sig_mask);
	pthread_sigmask(SIG_BLOCK, &sig_mask, NULL);

	if (pthread_create(&commands_thread, NULL, &commands_handler, NULL) != 0 ||
		pthread_create(&input_thread, NULL, &input_handler, NULL) != 0)
	{
		interrupt = 1;
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	// Setup signal handler for the main thread.
	sigemptyset(&sig_action.sa_mask);
	sigaddset(&sig_action.sa_mask, SIGCHLD);
	sigaddset(&sig_action.sa_mask, SIGINT);
	sig_action.sa_handler = sig_handler;
	sig_action.sa_flags = 0;

	if (sigaction(SIGCHLD, &sig_action, NULL) == -1 ||
		sigaction(SIGINT, &sig_action, NULL) == -1)
	{
		interrupt = 1;
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	pthread_sigmask(SIG_UNBLOCK, &sig_mask, NULL);

	// Wait for both handlers to finish.
	pthread_join(commands_thread, NULL);
	pthread_join(input_thread, NULL);

	exit(EXIT_SUCCESS);
}
