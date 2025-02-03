#include "abort.h"

#include <semaphore.h>

sem_t termination_semaphore;

size_t num_waiting;

void term(void)
{
	for (size_t i = 0; i < num_waiting; i++) {
		if (sem_post(&termination_semaphore) < 0)
			FATAL_ERR("term: sem_post: %s", STR_ERR);
	}
}

void term_init(size_t count)
{
	num_waiting = count;
	if (sem_init(&termination_semaphore, 0, 0) < 0)
		FATAL_ERR("term_init: sem_init: %s", STR_ERR);
}

void term_block(void)
{
	if (sem_wait(&termination_semaphore) < 0)
		FATAL_ERR("term_block: term_block: %s", STR_ERR);
}
