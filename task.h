// the task abi
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
typedef struct TaskHeader
{
  uint32_t taskIdLen;
  uint32_t taskCommandLen;
} TaskHeader;
typedef struct Task
{
  TaskHeader header;
  char *taskId;
  char *taskCommand;
} Task;

#endif // TASK_H
