#include "cuert_app.h"
#include "main.h"
#include <string.h>

extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef htim3;

#define RX_CAPACITY 256U
#define LINE_CAPACITY 64U
#define ACTUATE_FLAGS (APP_ACTUATE_FLAG_COMMAND_READY | APP_ACTUATE_FLAG_BRAKE_PENDING | \
                       APP_ACTUATE_FLAG_LINK_CHANGED | APP_ACTUATE_FLAG_TX_DONE | \
                       APP_ACTUATE_FLAG_RX_REJECTED | APP_ACTUATE_FLAG_TX_READY)

/* USART1 ISR produces bytes, COMMAND_RX consumes them. Task-side access uses
 * very short interrupt masking, NOT the task mutex. No RTOS call while masked.
 * A lost/corrupt byte invalidates the buffered stream until resynchronization.
 */
static uint8_t rx_byte;
static uint8_t rx_ring[RX_CAPACITY];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile bool rx_fault;

/* Task producers serialize ring publication with appTxMutexHandle. ACTUATE
 * alone advances the tail and starts TX; ISR only signals completion.
 * Bytes remain occupied/immutable until TX_DONE is processed.
 */
#define TX_CAPACITY 2048U
#define LOG_CAPACITY 192U
static char tx_ring[TX_CAPACITY];
static uint16_t tx_head, tx_tail, tx_count;
static uint16_t tx_in_flight; /* ACTUATE only. */
static char pending_log[LOG_CAPACITY]; /* One retained dequeue record. */
static uint16_t pending_log_length;
static uint32_t logged_loss_epoch; /* ACTUATE: one exact line per loss. */
static uint32_t blink_start_ms; /* ACTUATE; starts with a dark interval. */

static void StateLock(void)
{
    if (osMutexAcquire(appStateMutexHandle, osWaitForever) != osOK) {
        Error_Handler();
    }
}

static void StateUnlock(void)
{
    if (osMutexRelease(appStateMutexHandle) != osOK) {
        Error_Handler();
    }
}

static void Notify(osThreadId_t task, uint32_t flags)
{
    if ((osThreadFlagsSet(task, flags) & osFlagsError) != 0U) {
        Error_Handler();
    }
}

static uint32_t WaitFlags(uint32_t flags)
{
    uint32_t result = osThreadFlagsWait(flags, osFlagsWaitAny, osWaitForever);
    if ((result & osFlagsError) != 0U) {
        Error_Handler();
    }
    return result;
}

/* Exact uppercase verb, one ASCII space, optional sign, one or more decimal
 * digits, no trailing whitespace/tokens. Accumulation is bounded before it can
 * overflow even for an arbitrarily long numeric token. Never clamp.
 */
static bool ParseCommand(const char *line, Command_t *command)
{
    const char *number;
    bool negative = false;
    uint32_t magnitude = 0U;
    *command = (Command_t){0};
    if (strcmp(line, "PING") == 0) {
        command->type = 'P';
        return true;
    }
    if (strncmp(line, "THROTTLE ", 9U) == 0) {
        command->type = 'T';
        number = line + 9;
    } else if (strncmp(line, "STEER ", 6U) == 0) {
        command->type = 'S';
        number = line + 6;
    } else if (strncmp(line, "BRAKE ", 6U) == 0) {
        command->type = 'B';
        number = line + 6;
    } else {
        return false;
    }
    if (*number == '-' || *number == '+') {
        negative = (*number == '-');
        ++number;
    }
    if (*number == '\0') {
        return false;
    }
    for (; *number != '\0'; ++number) {
        if (*number < '0' || *number > '9') {
            return false;
        }
        magnitude = magnitude * 10U + (uint32_t)(*number - '0');
        if (magnitude > 100U) {
            return false;
        }
    }
    command->value = (int16_t)(negative ? -(int32_t)magnitude : (int32_t)magnitude);
    return command->type == 'S' || command->value >= 0;
}

static void SubmitCommand(Command_t *command)
{
    command->timestamp_ms = HAL_GetTick();
    StateLock();
    appState.last_valid_command_timestamp_ms = command->timestamp_ms;
    appState.has_valid_command = true;
    appState.last_command_type = command->type;
    appState.last_command_value = command->value;
    command->loss_epoch = appState.loss_epoch;
    command->throttle_blocked_at_rx = appState.brake_active ||
                                      appState.pending_positive_brakes != 0U;
    bool positive_brake = command->type == 'B' && command->value > 0;
    if (positive_brake) {
        /* One producer, bounded FIFO and one in-flight dequeue bound this
         * count by APP_COMMAND_QUEUE_LENGTH + 2; it cannot wrap. */
        ++appState.pending_positive_brakes;
    }
    StateUnlock();

    Notify(WATCHDOGHandle, APP_WATCHDOG_FLAG_VALID);
    if (positive_brake) {
        Notify(ACTUATEHandle, APP_ACTUATE_FLAG_BRAKE_PENDING);
        /* RX outranks ACTUATE. Block so ACTUATE can disarm and write zero now,
         * before RX attempts the FIFO or processes another buffered line.
         * One request/ack at a time; no stale ack and no shared mutex held.
         */
        (void)WaitFlags(APP_RX_FLAG_BRAKE_APPLIED);
    }
    if (osMessageQueuePut(commandQueueHandle, command, 0U, osWaitForever) != osOK) {
        /* Never undo an asserted brake or silently discard an accepted item. */
        Error_Handler();
    }
    Notify(ACTUATEHandle, APP_ACTUATE_FLAG_COMMAND_READY);
    if (command->type == 'B' && command->value == 0) {
        /* Finish release before validating the next buffered line. This makes
         * BRAKE 0 / THROTTLE bursts fresh without relaxing braking rejection. */
        (void)WaitFlags(APP_RX_FLAG_BRAKE_RELEASED);
    }
}

/* Returns byte, -1 for stream loss, or -2 for empty. */
static int RxPop(void)
{
    int result = -2;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (rx_fault) {
        rx_tail = rx_head;
        rx_fault = false;
        result = -1;
    } else if (rx_tail != rx_head) {
        result = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) % RX_CAPACITY);
    }
    __set_PRIMASK(mask);
    return result;
}

static void RejectLine(void)
{
    Notify(ACTUATEHandle, APP_ACTUATE_FLAG_RX_REJECTED | APP_ACTUATE_FLAG_TX_READY);
}

/* State belongs exclusively to COMMAND_RX; supports CR, LF and CRLF.
 * Empty terminators are ignored. Overlong/binary/damaged lines are discarded
 * through the next terminator, so a suffix can never become a command.
 */
static void ConsumeByte(int byte)
{
    static char line[LINE_CAPACITY];
    static uint32_t length;
    static bool discard;
    if (byte < 0) {
        length = 0U;
        discard = true;
        RejectLine();
    } else if (byte == '\r' || byte == '\n') {
        if (!discard && length != 0U) {
            Command_t command;
            line[length] = '\0';
            if (ParseCommand(line, &command)) {
                SubmitCommand(&command);
            } else {
                RejectLine();
            }
        }
        length = 0U;
        discard = false;
    } else if (!discard) {
        if (byte < 32 || byte > 126 || length == LINE_CAPACITY - 1U) {
            length = 0U;
            discard = true;
            RejectLine();
        } else {
            line[length++] = (char)byte;
        }
    }
}

void AppCommandRxRun(void)
{
    if (HAL_UART_Receive_IT(&huart1, &rx_byte, 1U) != HAL_OK) {
        Error_Handler();
    }
    for (;;) {
        uint32_t consumed = 0U;
        int byte;
        while (consumed < 64U && (byte = RxPop()) != -2) {
            ConsumeByte(byte);
            ++consumed;
        }
        if (consumed == 64U) {
            /* Bound high-priority work even with sustained invalid input.
             * Valid positive brakes complete the safety handshake first. */
            osDelay(1U);
        } else {
            (void)WaitFlags(APP_RX_FLAG_BYTES_READY);
        }
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart != &huart1) {
        return;
    }
    /* Do not rearm/clear ErrorCode before HAL's error branch has seen it. */
    if (uart->ErrorCode != HAL_UART_ERROR_NONE) {
        rx_fault = true;
    } else {
        uint16_t next = (uint16_t)((rx_head + 1U) % RX_CAPACITY);
        if (next == rx_tail) {
            rx_fault = true;
        } else if (!rx_fault) {
            rx_ring[rx_head] = rx_byte;
            rx_head = next;
        }
        if (HAL_UART_Receive_IT(uart, &rx_byte, 1U) != HAL_OK) {
            rx_fault = true;
        }
    }
    (void)osThreadFlagsSet(COMMAND_RXHandle, APP_RX_FLAG_BYTES_READY);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart1) {
        rx_fault = true;
        /* HAL aborts RX on overrun; other errors may leave reception active. */
        if (uart->RxState == HAL_UART_STATE_READY) {
            (void)HAL_UART_Receive_IT(uart, &rx_byte, 1U);
        }
        (void)osThreadFlagsSet(COMMAND_RXHandle, APP_RX_FLAG_BYTES_READY);
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart)
{
    if (uart == &huart1) {
        (void)osThreadFlagsSet(ACTUATEHandle, APP_ACTUATE_FLAG_TX_DONE);
    }
}

static bool LinkAllowsOutput(void)
{
    return appState.link_healthy;
}

/* ACTUATE only. Final state check AND CCR write are inside the SAME mutex
 * transaction, closing the check/unlock/write race with a new brake assertion.
 * This is only bounded state work and a register write, never a HAL wait.
 */
static uint16_t ApplyCommand(const Command_t *command)
{
    StateLock();
    uint32_t now = HAL_GetTick();
    if (appState.applied_loss_epoch != appState.loss_epoch) {
        appState.throttle_armed = false;
        appState.applied_loss_epoch = appState.loss_epoch;
        blink_start_ms = now;
    }
    if (appState.pending_positive_brakes != 0U || !LinkAllowsOutput()) {
        appState.throttle_armed = false;
    }
    if (command != NULL) {
        switch (command->type) {
        case 'T':
            appState.throttle_demand = command->value;
            appState.throttle_armed = !command->throttle_blocked_at_rx &&
                command->loss_epoch == appState.loss_epoch &&
                LinkAllowsOutput() && !appState.brake_active &&
                appState.pending_positive_brakes == 0U;
            break;
        case 'S':
            appState.steer_demand = command->value;
            break;
        case 'B':
            appState.brake_demand = command->value;
            appState.brake_active = command->value > 0;
            appState.throttle_armed = false;
            if (command->value > 0) {
                if (appState.pending_positive_brakes == 0U) {
                    Error_Handler(); /* Internal producer/consumer contract violation. */
                }
                --appState.pending_positive_brakes;
            }
            break;
        default: /* PING has no actuator effect. */
            break;
        }
    }
    uint16_t compare = 0U;
    if (LinkAllowsOutput() && appState.pending_positive_brakes == 0U &&
        !appState.brake_active && appState.throttle_armed) {
        compare = (uint16_t)(((htim3.Init.Period + 1U) *
                             (uint32_t)appState.throttle_demand) / 100U);
    }
    appState.output_percent = (uint8_t)(compare / 10U);
    /* One shared 100 ms OFF / 100 ms ON phase for both safe-idle LEDs.
     * Logical throttle stays zero/disarmed. Active or pending brakes override
     * PA6 even during loss; PC13 continues to indicate the lost link.
     * Existing flags interrupt the 10 ms loss wait for brake/recovery work. */
    bool braking = appState.brake_active || appState.pending_positive_brakes != 0U;
    bool watchdog_led_on = appState.link_lost &&
        (uint32_t)(now - blink_start_ms) % 200U >= 100U;
    if (watchdog_led_on && !braking) {
        compare = (uint16_t)(htim3.Init.Period + 1U);
    }
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13,
                     watchdog_led_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, compare);
    appState.pwm_compare = compare;
    StateUnlock();
    return compare;
}

/* Unsigned formatting also handles the full uint32_t uptime range. */
static char *AppendUnsigned(char *out, uint32_t value)
{
    char digits[10];
    unsigned int count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        *out++ = digits[--count];
    }
    return out;
}

static char *AppendNumber(char *out, int32_t value)
{
    if (value < 0) {
        *out++ = '-';
        return AppendUnsigned(out, 0U - (uint32_t)value);
    }
    return AppendUnsigned(out, (uint32_t)value);
}

static char *AppendText(char *out, const char *text)
{
    while (*text != '\0') {
        *out++ = *text++;
    }
    return out;
}

static AppState_t Snapshot(void)
{
    StateLock();
    AppState_t snapshot = appState;
    StateUnlock();
    return snapshot;
}

/* Maximum record is <192 bytes, including 10-digit uptime and epoch values.
 * Format only a copied snapshot, never under the state mutex. */
static uint16_t FormatRecord(char *buffer, const AppState_t *state,
                             const Command_t *command, bool status)
{
    char *out = AppendText(buffer, status ? "STATUS up_ms=" : "DEQ ");
    if (status) {
        out = AppendUnsigned(out, HAL_GetTick());
        out = AppendText(out, " last=");
        if (state->has_valid_command) {
            *out++ = state->last_command_type;
            *out++ = ':';
            out = AppendNumber(out, state->last_command_value);
            out = AppendText(out, " at_ms=");
            out = AppendUnsigned(out, state->last_valid_command_timestamp_ms);
        } else {
            out = AppendText(out, "NONE");
        }
    } else {
        *out++ = command->type;
        *out++ = ':';
        out = AppendNumber(out, command->value);
        out = AppendText(out, " rx_ms=");
        out = AppendUnsigned(out, command->timestamp_ms);
    }
    out = AppendText(out, " T=");
    out = AppendNumber(out, state->throttle_demand);
    out = AppendText(out, " S=");
    out = AppendNumber(out, state->steer_demand);
    out = AppendText(out, " B=");
    out = AppendNumber(out, state->brake_demand);
    out = AppendText(out, " OUT=");
    out = AppendUnsigned(out, state->output_percent);
    out = AppendText(out, " PWM=");
    out = AppendUnsigned(out, state->pwm_compare);
    out = AppendText(out, " armed=");
    out = AppendUnsigned(out, state->throttle_armed ? 1U : 0U);
    out = AppendText(out, " pending=");
    out = AppendUnsigned(out, state->pending_positive_brakes);
    out = AppendText(out, state->link_healthy ? " LINK=HEALTHY" :
                          (state->link_lost ? " LINK=LOST" : " LINK=STARTUP_WAIT"));
    out = AppendText(out, "\r\n");
    if (command != NULL && command->type == 'P') {
        out = AppendText(out, "OK\r\n");
    }
    return (uint16_t)(out - buffer);
}

static void TxLock(void)
{
    if (osMutexAcquire(appTxMutexHandle, osWaitForever) != osOK) {
        Error_Handler();
    }
}
static void TxUnlock(void)
{
    if (osMutexRelease(appTxMutexHandle) != osOK) {
        Error_Handler();
    }
}

/* No waiting for capacity. On false the caller RETAINS the complete record.
 * Separate PI mutex; no state mutex or interrupt masking across the copy. */
static bool TxAppend(const char *message, uint16_t length)
{
    TxLock();
    if (length > TX_CAPACITY - tx_count) {
        TxUnlock();
        return false;
    }
    for (uint16_t i = 0U; i < length; ++i) {
        tx_ring[tx_head] = message[i];
        tx_head = (uint16_t)((tx_head + 1U) % TX_CAPACITY);
    }
    tx_count = (uint16_t)(tx_count + length);
    TxUnlock();
    Notify(ACTUATEHandle, APP_ACTUATE_FLAG_TX_READY);
    return true;
}

/* ACTUATE only. ISR never touches the ring or calls a mutex. */
static void ServiceTx(bool completed)
{
    TxLock();
    if (completed) {
        tx_tail = (uint16_t)((tx_tail + tx_in_flight) % TX_CAPACITY);
        tx_count = (uint16_t)(tx_count - tx_in_flight);
        tx_in_flight = 0U;
    }
    uint16_t start = tx_tail;
    bool start_tx = tx_in_flight == 0U && tx_count != 0U;
    if (start_tx) {
        tx_in_flight = (uint16_t)(TX_CAPACITY - tx_tail);
        if (tx_in_flight > tx_count) {
            tx_in_flight = tx_count;
        }
        if (tx_in_flight > 128U) {
            tx_in_flight = 128U;
        }
    }
    TxUnlock();
    if (completed) {
        Notify(STATUSHandle, APP_STATUS_FLAG_TX_SPACE);
    }
    if (start_tx) {
        /* Serialize only HAL's brief TX setup against its RX/error callbacks. */
        uint32_t mask = __get_PRIMASK();
        __disable_irq();
        HAL_StatusTypeDef result = HAL_UART_Transmit_IT(&huart1,
                                 (uint8_t *)&tx_ring[start], tx_in_flight);
        __set_PRIMASK(mask);
        if (result != HAL_OK) {
            Error_Handler();
        }
    }
}

/* Called before dequeue, after physical safety has been applied. Loss epoch
 * survives flag coalescing and even loss/recovery before ACTUATE runs. */
static bool FlushRequiredLogs(void)
{
    AppState_t state = Snapshot();
    while (logged_loss_epoch != state.applied_loss_epoch) {
        static const char loss_message[] = "LINK LOST, failing safe\r\n";
        if (!TxAppend(loss_message, sizeof(loss_message) - 1U)) {
            return false;
        }
        ++logged_loss_epoch;
    }
    if (pending_log_length != 0U) {
        if (!TxAppend(pending_log, pending_log_length)) {
            return false;
        }
        pending_log_length = 0U;
    }
    return true;
}

static uint32_t TimedFlags(uint32_t mask, uint32_t ticks)
{
    uint32_t flags = osThreadFlagsWait(mask, osFlagsWaitAny, ticks);
    if (flags == osFlagsErrorTimeout || flags == osFlagsErrorResource) {
        return 0U;
    }
    if ((flags & osFlagsError) != 0U) {
        Error_Handler();
    }
    return flags;
}

void AppActuateRun(void)
{
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0U);
    __HAL_TIM_DISABLE_OCxPRELOAD(&htim3, TIM_CHANNEL_1);
    if (HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }
    uint32_t flags = 0U;
    for (;;) {
        flags |= TimedFlags(ACTUATE_FLAGS, 0U);
        (void)ApplyCommand(NULL);
        if ((flags & APP_ACTUATE_FLAG_BRAKE_PENDING) != 0U) {
            Notify(COMMAND_RXHandle, APP_RX_FLAG_BRAKE_APPLIED);
        }
        ServiceTx((flags & APP_ACTUATE_FLAG_TX_DONE) != 0U);
        flags = 0U;
        bool log_space = FlushRequiredLogs();
        AppState_t state = Snapshot();
        /* Do not discard a recovery throttle while the lower-priority
         * WATCHDOG still needs to handle RX's valid-command notification.
         * Startup uses the same ordering. Even with a full FIFO or TX ring,
         * safety/ACK above still run, then this task blocks on flags/timeouts. */
        if (state.link_healthy && log_space) {
            Command_t command;
            osStatus_t result = osMessageQueueGet(commandQueueHandle, &command, NULL, 0U);
            if (result == osOK) {
                (void)ApplyCommand(&command);
                state = Snapshot();
                if (command.type == 'B' && command.value == 0) {
                    Notify(COMMAND_RXHandle, APP_RX_FLAG_BRAKE_RELEASED);
                }
                pending_log_length = FormatRecord(pending_log, &state, &command, false);
                continue;
            }
            if (result != osErrorResource) {
                Error_Handler();
            }
        }
        /* 10 ms timeout only during loss drives the 100/100 ms LED pattern.
         * Healthy/startup task sleeps until real work is signalled. */
        flags = TimedFlags(ACTUATE_FLAGS, state.link_lost ? 10U : osWaitForever);
    }
}

/* WATCHDOG alone writes link_healthy, link_lost and loss_epoch. The initial
 * timestamp is set before scheduling; has_valid_command distinguishes grace
 * from a healthy link. No UART work or actuator work in this task. */
static void WatchdogStep(void)
{
    StateLock();
    uint32_t elapsed = (uint32_t)(HAL_GetTick() - appState.last_valid_command_timestamp_ms);
    bool lost = elapsed > APP_LINK_TIMEOUT_MS;
    bool healthy = !lost && appState.has_valid_command;
    bool changed = lost != appState.link_lost || healthy != appState.link_healthy;
    if (lost && !appState.link_lost) {
        ++appState.loss_epoch;
    }
    appState.link_lost = lost;
    appState.link_healthy = healthy;
    StateUnlock();
    if (changed) {
        Notify(ACTUATEHandle, APP_ACTUATE_FLAG_LINK_CHANGED);
    }
}

void AppWatchdogRun(void)
{
    for (;;) {
        WatchdogStep();
        (void)TimedFlags(APP_WATCHDOG_FLAG_VALID, 20U);
    }
}

void AppStatusRun(void)
{
    uint32_t next = osKernelGetTickCount();
    for (;;) {
        next += 1000U; /* Configured CMSIS/FreeRTOS tick is 1 kHz. */
        (void)osDelayUntil(next);
        AppState_t state = Snapshot();
        char message[LOG_CAPACITY];
        uint16_t length = FormatRecord(message, &state, NULL, true);
        while (!TxAppend(message, length)) {
            (void)WaitFlags(APP_STATUS_FLAG_TX_SPACE);
        }
        /* After exceptional UART backlog, skip missed slots instead of
         * generating a burst of overdue STATUS lines. */
        uint32_t now = osKernelGetTickCount();
        if ((uint32_t)(now - next) >= 1000U) {
            next = now;
        }
    }
}
