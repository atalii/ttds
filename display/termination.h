#pragma once

void term_init(int);
void term_block(void);

/* Signal termination to all threads waiting for it. This does *not* properly
terminate the process; send SIGINT instead. */
void term(void);
