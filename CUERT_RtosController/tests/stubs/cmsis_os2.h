#ifndef TEST_CMSIS_OS2_H
#define TEST_CMSIS_OS2_H
#include <stdint.h>
typedef void *osThreadId_t;
typedef void *osMutexId_t;
typedef void *osMessageQueueId_t;
typedef int32_t osStatus_t;
#define osOK 0
#define osErrorResource (-3)
#define osWaitForever 0xffffffffU
#define osFlagsError 0x80000000U
#define osFlagsErrorTimeout 0xfffffffeU
#define osFlagsErrorResource 0xfffffffdU
#define osFlagsWaitAny 0U
osStatus_t osMutexAcquire(osMutexId_t mutex, uint32_t timeout);
osStatus_t osMutexRelease(osMutexId_t mutex);
uint32_t osThreadFlagsSet(osThreadId_t task, uint32_t flags);
uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout);
osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void *item,
                             uint8_t priority, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void *item,
                             uint8_t *priority, uint32_t timeout);
osStatus_t osDelay(uint32_t ticks);
uint32_t osKernelGetTickCount(void);
osStatus_t osDelayUntil(uint32_t tick);
#endif
