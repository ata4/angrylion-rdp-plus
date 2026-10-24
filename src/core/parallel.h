#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define PARALLEL_MAX_WORKERS 64u

// mailbox for values one worker produces and another one waits for:
// PARALLEL_MAILBOX_LINES x PARALLEL_MAILBOX_SEQS slots
#define PARALLEL_MAILBOX_LINES 1024u
#define PARALLEL_MAILBOX_SEQS 1024u

void parallel_init(uint32_t num, bool busy);
void parallel_run(void task(uint32_t));
uint32_t parallel_num_workers();
void parallel_close();

// store value in slot, tagged with seq (seq must not be 0)
void parallel_mailbox_post(uint32_t slot, uint32_t seq, uint32_t value);
// wait until slot holds a value tagged with seq and return it
uint32_t parallel_mailbox_wait(uint32_t slot, uint32_t seq);

#ifdef __cplusplus
}
#endif
