/*
 * Author  : Jakub Šoustar <jakub.soustar@gmail.com> <xsoust02@stud.fit.vutbr.cz>
 * Project : Signals
 *
*/

#define _XOPEN_SOURCE 500

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

volatile sig_atomic_t interrupt;
sig_atomic_t curr_char;

char next_char() {
	char old_char = curr_char++;

	if (curr_char > 'Z') {
		curr_char = 'A';
	}

	return old_char;
}

void sig_handler(int sig_num) {
	if (sig_num == SIGUSR1) {
		interrupt = 1;
		return;
	}

	if (sig_num == SIGUSR2) {
		curr_char = 'A';
		return;
	}
}

int main() {
	struct sigaction sig_action;
	sigset_t mask_block;
	sigset_t mask_empty;

	int is_parent = 1;
	int do_prompt = 0;
	pid_t m_pid;
	pid_t t_pid;

	sigemptyset(&mask_empty);
	sigemptyset(&mask_block);
	sigaddset(&mask_block, SIGUSR1);
	sigaddset(&mask_block, SIGUSR2);

	sig_action.sa_handler = sig_handler;
	sig_action.sa_mask = mask_block;
	sig_action.sa_flags = 0;

	if (sigaction(SIGUSR1, &sig_action, NULL) < 0 ||
		sigaction(SIGUSR2, &sig_action, NULL) < 0)
	{
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	sigprocmask(SIG_BLOCK, &mask_block, NULL);

	curr_char = 'A';
	t_pid = fork();

	if (t_pid == 0) {
		t_pid = getppid();
		is_parent = 0;
	}

	m_pid = getpid();

	while (1) {
		if (! is_parent || do_prompt) {
			while (1) {
				sigsuspend(&mask_empty);

				if (interrupt && errno == EINTR) {
					interrupt = 0;
					break;
				}
			}
		}

		if (is_parent) {
			if (do_prompt) {
				printf("Press enter...");
				while (getchar() != '\n');
			} else {
				do_prompt = 1;
			}
		}

		sigprocmask(SIG_BLOCK, &mask_block, NULL);
		printf("%s (%d): '%c'\n", is_parent ? "Parent" : "Child", (int) m_pid, next_char());
		sigprocmask(SIG_UNBLOCK, &mask_block, NULL);

		kill(t_pid, SIGUSR1);
	}

	exit(EXIT_SUCCESS);
}
