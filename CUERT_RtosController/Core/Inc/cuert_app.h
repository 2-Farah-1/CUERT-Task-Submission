#ifndef CUERT_APP_H
#define CUERT_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "cmsis_os2.h"

typedef struct {
    char type;                 /* 'T', 'S', 'B', 'P'; validated commands only. */
    int16_t value;
    uint32_t timestamp_ms;      /* HAL ms tick at complete command validation. */
    uint32_t loss_epoch;        /* Reject pre-fail-safe queued throttle after recovery. */
    bool throttle_blocked_at_rx; /* Preserve braking context across FIFO delays. */
} Command_t;

#define APP_COMMAND_QUEUE_LENGTH 16U
#define APP_LINK_TIMEOUT_MS 500U

/* Reserved CMSIS thread flags on the existing generated task handles.
 * Flags are wake hints, never counts: pending_positive_brakes is authoritative.
 */
#define APP_RX_FLAG_BYTES_READY       (1UL << 0)
#define APP_RX_FLAG_BRAKE_APPLIED     (1UL << 1)
#define APP_RX_FLAG_BRAKE_RELEASED    (1UL << 2)
#define APP_WATCHDOG_FLAG_VALID       (1UL << 0)
#define APP_STATUS_FLAG_TX_SPACE      (1UL << 0)
#define APP_ACTUATE_FLAG_COMMAND_READY (1UL << 0)
#define APP_ACTUATE_FLAG_BRAKE_PENDING (1UL << 1)
#define APP_ACTUATE_FLAG_LINK_CHANGED  (1UL << 2)
#define APP_ACTUATE_FLAG_TX_DONE       (1UL << 3)
#define APP_ACTUATE_FLAG_RX_REJECTED   (1UL << 4)
#define APP_ACTUATE_FLAG_TX_READY      (1UL << 5)

/* Every task read/write of this state must hold appStateMutexHandle.
 * STATUS takes a snapshot under the mutex, then releases it before logging.
 * No ISR accesses this state or mutex. Do not hold the mutex while waiting
 * for queue space, flags, UART, delays, or other blocking operations.
 */
typedef struct {
    uint32_t last_valid_command_timestamp_ms; /* COMMAND_RX writes after startup initialization. */
    bool has_valid_command;                  /* COMMAND_RX writes after startup initialization. */
    char last_command_type;                  /* COMMAND_RX writes. */
    int16_t last_command_value;              /* COMMAND_RX writes. */
    bool link_lost;                          /* WATCHDOG; false during startup. */
    uint32_t loss_epoch;                     /* WATCHDOG increments on loss. */
    uint32_t applied_loss_epoch;             /* ACTUATE acknowledges/disarms. */
    uint8_t output_percent;                 /* ACTUATE: logical output, zero during blink. */
    bool link_healthy;                       /* WATCHDOG alone writes. */
    uint32_t pending_positive_brakes;         /* RX increments; ACTUATE decrements. */
    bool brake_active;                       /* ACTUATE alone writes. */
    bool throttle_armed;                     /* ACTUATE alone writes. */
    int16_t throttle_demand;                  /* ACTUATE alone writes; 0..100. */
    int16_t steer_demand;                     /* ACTUATE alone writes; -100..100. */
    int16_t brake_demand;                     /* ACTUATE alone writes; 0..100. */
    uint16_t pwm_compare;                     /* ACTUATE publishes applied CCR. */
} AppState_t;

/* Created once before the scheduler starts; handles remain immutable.
 * FIFO producer: COMMAND_RX; consumer: ACTUATE. Always use message priority 0.
 * RX asserts positive brake under the state mutex, releases it,
 * notifies ACTUATE and waits for safe-zero acknowledgement, THEN enqueues.
 * BRAKE 0 never decrements pending_positive_brakes. Only ACTUATE acknowledges
 * each dequeued positive brake. With one producer and this bounded FIFO, at
 * most queue length + 2 assertions can be in flight (including both tasks).
 * Pending brake forbids output independently of throttle_armed; ACTUATE
 * disarms on the wake. CCR writes share the same mutex as the final gate.
 */
extern osMessageQueueId_t commandQueueHandle;
extern osMutexId_t appStateMutexHandle;
extern osMutexId_t appTxMutexHandle; /* TX ring only; never nested with state mutex. */
extern AppState_t appState;

/* Reuse CubeMX definitions in main.c; these declarations create no tasks. */
extern osThreadId_t COMMAND_RXHandle;
extern osThreadId_t ACTUATEHandle;
extern osThreadId_t WATCHDOGHandle;
extern osThreadId_t STATUSHandle;

void AppCommandRxRun(void);
void AppActuateRun(void);
void AppWatchdogRun(void);
void AppStatusRun(void);

#endif /* CUERT_APP_H */
