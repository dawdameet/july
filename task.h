// the task abi
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TASK_ID 64
#define MAX_TASK_CMD 512

typedef struct TaskHeader
{
  uint32_t taskIdLen;
  uint32_t taskCommandLen;
} TaskHeader;

typedef struct Task
{
  TaskHeader header;
  char taskId[MAX_TASK_ID];
  char taskCommand[MAX_TASK_CMD];
} Task;

#endif // TASK_H
