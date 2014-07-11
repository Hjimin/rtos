#ifndef __ICC_H__
#define __ICC_H__

#include <stdint.h>
#include "vm.h"

typedef enum {
	ICC_TYPE_START = 1,
	ICC_TYPE_STARTED,
	ICC_TYPE_PAUSE,
	ICC_TYPE_PAUSEPED,
	ICC_TYPE_STOP,
	ICC_TYPE_STOPPED,
} ICCType;

#define ICC_STATUS_DONE		0
#define ICC_STATUS_SENT		1
#define ICC_STATUS_RECEIVED	2

typedef struct _ICC_Message {
	uint32_t	id;
	uint8_t		type;
	uint8_t		core_id;
	volatile int	status;
	int		result;
	
	union {
		struct {
			VM*	vm;
		} start;
		
		struct {
			char*	stdin;
			size_t*	stdin_head;
			size_t*	stdin_tail;
			size_t	stdin_size;
			char*	stdout;
			size_t*	stdout_head;
			size_t*	stdout_tail;
			size_t	stdout_size;
			char*	stderr;
			size_t*	stderr_head;
			size_t*	stderr_tail;
			size_t	stderr_size;
		} execute;
	} data;
} ICC_Message;

void icc_init();
ICC_Message* icc_sending(uint8_t type, uint16_t core_id);
uint32_t icc_send(ICC_Message* msg);
void icc_register(uint8_t type, void(*event)(ICC_Message*));

#endif /* __ICC_H__ */