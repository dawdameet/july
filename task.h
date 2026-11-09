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

typedef enum {
    TASK_TYPE_COMMAND = 0,
    TASK_TYPE_WORKFLOW = 1
} TaskType;

typedef struct Task
{
  TaskHeader header;
  TaskType type;
  char taskId[MAX_TASK_ID];
  char taskCommand[MAX_TASK_CMD];
  char storageDir[256];
  char sandboxDir[256];
} Task;

#endif // TASK_H
