/* Actual application functions with deterministic HAL/RTOS doubles.
 * Checks state/order contracts; not a real scheduler or board test.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../Core/Src/cuert_app.c"

UART_HandleTypeDef huart1;
TIM_HandleTypeDef htim3 = {{999U}};
AppState_t appState;
osMutexId_t appStateMutexHandle = (void *)1;
osMutexId_t appTxMutexHandle = (void *)7;
osMessageQueueId_t commandQueueHandle = (void *)2;
osThreadId_t ACTUATEHandle = (void *)3;
osThreadId_t COMMAND_RXHandle = (void *)4;
osThreadId_t WATCHDOGHandle = (void *)5;
osThreadId_t STATUSHandle = (void *)6;
static Command_t queue_items[APP_COMMAND_QUEUE_LENGTH];
static unsigned int queue_count, lock_depth, hardware_compare;
static GPIO_PinState watchdog_led = GPIO_PIN_SET;
static uint32_t ticks, irq_mask, rx_flags, reject_count, receive_calls;
static unsigned int full_brake_puts, ack_waits, tx_calls;
static bool simulate_full_queue;
static unsigned int tx_lock_depth;
static bool auto_watchdog = true;
static uint8_t tx_copy[128];
static uint16_t tx_size;
static void Drain(void);

void Error_Handler(void) { assert(!"Unexpected Error_Handler"); abort(); }
uint32_t HAL_GetTick(void) { return ticks; }
uint32_t __get_PRIMASK(void) { return irq_mask; }
void __disable_irq(void) { irq_mask = 1U; }
void __set_PRIMASK(uint32_t mask) { irq_mask = mask; }
void HAL_GPIO_WritePin(void *port, uint16_t pin, GPIO_PinState state)
{
    assert(port == GPIOC && pin == GPIO_PIN_13 && lock_depth == 1U);
    watchdog_led = state;
}
void TestSetCompare(uint32_t compare)
{
    assert(lock_depth == 1U && compare <= 1000U);
    if (compare != 0U) {
        assert(!appState.brake_active);
        assert(appState.pending_positive_brakes == 0U);
        assert((!appState.brake_active && appState.throttle_armed && appState.link_healthy) ||
               (appState.link_lost && appState.output_percent == 0U && !appState.throttle_armed));
    }
    hardware_compare = compare;
}
osStatus_t osMutexAcquire(osMutexId_t mutex, uint32_t timeout)
{
    assert(timeout == osWaitForever);
    if (mutex == appTxMutexHandle) {
        assert(lock_depth == 0U && tx_lock_depth == 0U && irq_mask == 0U);
        tx_lock_depth = 1U;
        return osOK;
    }
    assert(mutex == appStateMutexHandle && tx_lock_depth == 0U);
    assert(lock_depth == 0U && irq_mask == 0U);
    lock_depth = 1U;
    return osOK;
}
osStatus_t osMutexRelease(osMutexId_t mutex)
{
    if (mutex == appTxMutexHandle) {
        assert(lock_depth == 0U && tx_lock_depth == 1U);
        tx_lock_depth = 0U;
        return osOK;
    }
    assert(mutex == appStateMutexHandle && lock_depth == 1U);
    lock_depth = 0U;
    return osOK;
}
uint32_t osThreadFlagsSet(osThreadId_t task, uint32_t flags)
{
    assert(lock_depth == 0U && irq_mask == 0U);
    if (task == WATCHDOGHandle && auto_watchdog) {
        WatchdogStep();
    }
    if (task == ACTUATEHandle && (flags & APP_ACTUATE_FLAG_BRAKE_PENDING) != 0U) {
        assert(appState.pending_positive_brakes != 0U);
        (void)ApplyCommand(NULL); /* Schedule actuator safety pass. */
        rx_flags |= APP_RX_FLAG_BRAKE_APPLIED;
    }
    if (task == ACTUATEHandle && (flags & APP_ACTUATE_FLAG_RX_REJECTED) != 0U) {
        ++reject_count;
    }
    return flags;
}
uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    assert(lock_depth == 0U && irq_mask == 0U && options == osFlagsWaitAny);
    if (flags == APP_RX_FLAG_BRAKE_APPLIED && timeout == osWaitForever) {
        assert((rx_flags & flags) != 0U);
        assert(hardware_compare == 0U && !appState.throttle_armed);
        rx_flags &= ~flags;
        ++ack_waits;
        return flags;
    }
    if (flags == APP_RX_FLAG_BRAKE_RELEASED && timeout == osWaitForever) {
        Drain(); /* Schedule ordered release processing before RX continues. */
        assert(!appState.brake_active && !appState.throttle_armed && hardware_compare == 0U);
        return flags;
    }
    return osFlagsErrorResource;
}
osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void *item,
                             uint8_t *priority, uint32_t timeout)
{
    (void)priority;
    assert(queue == commandQueueHandle && timeout == 0U && lock_depth == 0U);
    if (queue_count == 0U) { return osErrorResource; }
    *(Command_t *)item = queue_items[0];
    --queue_count;
    memmove(queue_items, queue_items + 1, queue_count * sizeof(Command_t));
    return osOK;
}
osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void *item,
                             uint8_t priority, uint32_t timeout)
{
    assert(queue == commandQueueHandle && priority == 0U);
    assert(timeout == osWaitForever && lock_depth == 0U && irq_mask == 0U);
    if (queue_count == APP_COMMAND_QUEUE_LENGTH) {
        assert(simulate_full_queue);
        assert(((const Command_t *)item)->type == 'B');
        assert(appState.pending_positive_brakes != 0U);
        assert(hardware_compare == 0U && !appState.throttle_armed);
        Command_t oldest;
        assert(osMessageQueueGet(queue, &oldest, NULL, 0U) == osOK);
        (void)ApplyCommand(&oldest);
        assert(hardware_compare == 0U);
        ++full_brake_puts;
    }
    queue_items[queue_count++] = *(const Command_t *)item;
    return osOK;
}
uint32_t osKernelGetTickCount(void) { return ticks; }
osStatus_t osDelayUntil(uint32_t tick) { ticks = tick; return osOK; }
osStatus_t osDelay(uint32_t delay)
{
    assert(lock_depth == 0U && irq_mask == 0U && delay != 0U);
    return osOK;
}
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *uart, uint8_t *data, uint16_t size)
{
    assert(uart == &huart1 && data == &rx_byte && size == 1U);
    uart->RxState = HAL_UART_STATE_BUSY_RX;
    uart->ErrorCode = HAL_UART_ERROR_NONE;
    ++receive_calls;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t size)
{
    assert(uart == &huart1 && lock_depth == 0U && tx_lock_depth == 0U && size <= sizeof(tx_copy));
    assert(tx_in_flight == size);
    memcpy(tx_copy, data, size);
    tx_size = size;
    ++tx_calls;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *timer, uint32_t channel)
{
    assert(timer == &htim3 && channel == TIM_CHANNEL_1 && lock_depth == 0U);
    return HAL_OK;
}
static void Feed(const char *input)
{
    for (; *input != '\0'; ++input) { ConsumeByte((unsigned char)*input); }
}
static void Drain(void)
{
    Command_t command;
    while (osMessageQueueGet(commandQueueHandle, &command, NULL, 0U) == osOK) {
        (void)ApplyCommand(&command);
    }
}
static void Reset(void)
{
    appState = (AppState_t){0};
    queue_count = hardware_compare = reject_count = rx_flags = 0U;
    tx_head = tx_tail = tx_count = tx_in_flight = pending_log_length = 0U;
    logged_loss_epoch = blink_start_ms = 0U;
    auto_watchdog = true;
    rx_head = rx_tail = 0U;
    rx_fault = false;
    huart1 = (UART_HandleTypeDef){0};
    simulate_full_queue = false;
    ticks = 123U;
    ConsumeByte('\n');
}
static void TestParser(void)
{
    const char *valid[] = {"PING", "THROTTLE 0", "THROTTLE 100", "STEER -100",
                           "STEER +100", "BRAKE 0", "BRAKE 100", "STEER -0"};
    const char *invalid[] = {"", "ping", "PING 0", "PING ", "THROTTLE", "THROTTLE ",
        "THROTTLE -1", "THROTTLE 101", "STEER -101", "BRAKE 101", "BRAKE -1",
        "STEER +", "BRAKE --1", "THROTTLE 1.0", "THROTTLE 1 x",
        "THROTTLE  1", "STEER\t1", " STEER 1", "BRAKE NaN",
        "THROTTLE 4294967296", "STEER 999999999999999999999999999999"};
    Command_t command;
    for (unsigned int i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(ParseCommand(valid[i], &command));
    }
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!ParseCommand(invalid[i], &command));
    }
}
static void TestLinesAndLiveness(void)
{
    Reset();
    appState.last_valid_command_timestamp_ms = 77U;
    Feed("bad\r\nTHROTTLE 101\nPING extra\n");
    assert(queue_count == 0U && !appState.has_valid_command);
    assert(appState.last_valid_command_timestamp_ms == 77U);
    Feed("PING\r\nSTEER -100\rTHROTTLE 100\n");
    assert(queue_count == 3U);
    assert(queue_items[0].type == 'P' && queue_items[1].type == 'S');
    assert(queue_items[2].type == 'T' && queue_items[2].timestamp_ms == ticks);
    assert(appState.has_valid_command && appState.link_healthy);
    assert(appState.last_valid_command_timestamp_ms == ticks);
    Drain();
    assert(hardware_compare == 1000U && appState.steer_demand == -100);
    ticks = UINT32_MAX;
    Feed("PING\n");
    assert(appState.last_valid_command_timestamp_ms == UINT32_MAX);
    ticks = 0U;
    Feed("PING\n");
    assert(appState.last_valid_command_timestamp_ms == 0U);
}
static void TestOverflowAndErrors(void)
{
    Reset();
    for (unsigned int i = 0; i < 100U; ++i) { ConsumeByte('X'); }
    Feed("THROTTLE 100\nPING\n");
    assert(queue_count == 1U && queue_items[0].type == 'P');
    Drain();
    Feed("THROTTLE ");
    ConsumeByte(0);
    Feed("100\nPING\n");
    assert(queue_count == 1U && queue_items[0].type == 'P');
    Drain();
    Feed("THROTTLE ");
    for (unsigned int i = 0; i < RX_CAPACITY; ++i) {
        rx_byte = '0';
        HAL_UART_RxCpltCallback(&huart1);
    }
    assert(rx_fault);
    ConsumeByte(RxPop());
    assert(RxPop() == -2);
    Feed("100\nPING\n");
    assert(queue_count == 1U && queue_items[0].type == 'P');
    unsigned int previous_calls = receive_calls;
    huart1.ErrorCode = HAL_UART_ERROR_ORE;
    HAL_UART_RxCpltCallback(&huart1);
    assert(receive_calls == previous_calls && rx_fault);
    huart1.RxState = HAL_UART_STATE_READY;
    HAL_UART_ErrorCallback(&huart1);
    assert(receive_calls == previous_calls + 1U);
    assert(huart1.ErrorCode == HAL_UART_ERROR_NONE);
}
static void TestBrakes(void)
{
    Reset();
    Feed("THROTTLE 80\n");
    Drain();
    assert(hardware_compare == 800U && appState.throttle_armed);
    Feed("BRAKE 10\nBRAKE 20\nTHROTTLE 90\n");
    assert(appState.pending_positive_brakes == 2U);
    assert(hardware_compare == 0U && !appState.throttle_armed);
    Command_t command;
    assert(osMessageQueueGet(commandQueueHandle, &command, NULL, 0U) == osOK);
    ApplyCommand(&command);
    assert(appState.pending_positive_brakes == 1U && appState.brake_active);
    Drain();
    assert(appState.pending_positive_brakes == 0U && appState.brake_active);
    assert(appState.throttle_demand == 90 && !appState.throttle_armed);
    Feed("THROTTLE 85\n");
    Command_t stale = queue_items[0];
    assert(stale.throttle_blocked_at_rx);
    Drain();
    Feed("BRAKE 0\n");
    assert(!appState.brake_active && hardware_compare == 0U);
    ApplyCommand(&stale);
    assert(!appState.throttle_armed && hardware_compare == 0U);
    Feed("THROTTLE 60\n");
    Drain();
    assert(hardware_compare == 600U);
    Feed("BRAKE 100\nBRAKE 0\nTHROTTLE 55\n");
    Drain();
    assert(hardware_compare == 550U); /* Release handshake makes next line fresh. */
    Feed("BRAKE 100\nTHROTTLE 100\n");
    Drain();
    assert(hardware_compare == 0U && appState.brake_active);
}
static void TestFullQueue(void)
{
    Reset();
    Feed("THROTTLE 100\n");
    Drain();
    for (unsigned int i = 0; i < APP_COMMAND_QUEUE_LENGTH; ++i) {
        Feed("THROTTLE 80\n");
    }
    assert(queue_count == APP_COMMAND_QUEUE_LENGTH);
    simulate_full_queue = true;
    Feed("BRAKE 100\n");
    assert(full_brake_puts == 1U);
    assert(queue_count == APP_COMMAND_QUEUE_LENGTH);
    assert(queue_items[APP_COMMAND_QUEUE_LENGTH - 1U].type == 'B');
    Drain();
    assert(appState.pending_positive_brakes == 0U && appState.brake_active);
    assert(hardware_compare == 0U);
}
static void TestWatchdog(void)
{
    Reset();
    ticks = 0U;
    WatchdogStep();
    assert(!appState.link_lost && !appState.link_healthy && appState.loss_epoch == 0U);
    ticks = 500U;
    WatchdogStep();
    assert(!appState.link_lost);
    ticks = 501U;
    WatchdogStep();
    assert(appState.link_lost && appState.loss_epoch == 1U);
    ApplyCommand(NULL);
    assert(!appState.throttle_armed && hardware_compare == 0U);
    assert(FlushRequiredLogs());
    uint16_t count = tx_count;
    assert(count == sizeof("LINK LOST, failing safe\r\n") - 1U);
    WatchdogStep();
    assert(FlushRequiredLogs() && tx_count == count);
    ticks = 1401U;
    ApplyCommand(NULL);
    assert(hardware_compare == 1000U && appState.output_percent == 0U);
    assert(watchdog_led == GPIO_PIN_RESET);
    ticks = 1501U;
    ApplyCommand(NULL);
    assert(hardware_compare == 0U);
    Feed("THROTTLE abc\n");
    assert(appState.link_lost && !appState.has_valid_command);

    /* Explicitly delay lower-priority WATCHDOG: queued recovery T survives. */
    auto_watchdog = false;
    Feed("THROTTLE 20\n");
    assert(appState.link_lost && queue_count == 1U);
    assert(queue_items[0].loss_epoch == appState.loss_epoch);
    ApplyCommand(NULL);
    assert(!LinkAllowsOutput());
    WatchdogStep();
    assert(appState.link_healthy && !appState.link_lost);
    Drain();
    assert(hardware_compare == 200U && appState.throttle_armed);

    ticks += 501U;
    WatchdogStep();
    ApplyCommand(NULL);
    Feed("PING\n");
    WatchdogStep();
    Drain();
    assert(hardware_compare == 0U && !appState.throttle_armed);
    ticks += 501U;
    WatchdogStep();
    ApplyCommand(NULL);
    Feed("STEER -60\n");
    WatchdogStep();
    Drain();
    assert(hardware_compare == 0U && appState.steer_demand == -60);

    /* A queued pre-loss T never becomes a fresh recovery demand, even when
     * loss/recovery coalesce before ACTUATE acknowledges the epoch. */
    Feed("THROTTLE 90\n");
    ticks += 501U;
    WatchdogStep();
    Feed("PING\n");
    WatchdogStep();
    Drain();
    assert(hardware_compare == 0U && !appState.throttle_armed);
    appState.last_valid_command_timestamp_ms = UINT32_MAX - 200U;
    appState.has_valid_command = true;
    ticks = 299U;
    WatchdogStep();
    assert(appState.link_healthy); /* elapsed == 500 across wrap */
    ticks = 300U;
    WatchdogStep();
    assert(appState.link_lost); /* elapsed == 501 */
}
static void TestWatchdogLed(void)
{
    Reset();
    ticks = 0U;
    Feed("THROTTLE 40\n");
    Drain();
    assert(hardware_compare == 400U && watchdog_led == GPIO_PIN_SET);
    ticks = 501U;
    WatchdogStep();
    /* Sample every millisecond over ten periods, including boundaries. */
    for (; ticks < 2501U; ++ticks) {
        ApplyCommand(NULL);
        assert(appState.output_percent == 0U && !appState.throttle_armed);
        assert(hardware_compare == ((ticks - 501U) % 200U >= 100U ? 1000U : 0U));
        assert(watchdog_led == ((ticks - 501U) % 200U >= 100U ?
                               GPIO_PIN_RESET : GPIO_PIN_SET));
    }
    ticks = 3401U;
    ApplyCommand(NULL);
    assert(watchdog_led == GPIO_PIN_RESET);
    Feed("PING\n");
    Drain();
    assert(watchdog_led == GPIO_PIN_SET && hardware_compare == 0U);
    assert(!appState.throttle_armed); /* PING cannot rearm stale throttle. */
    Feed("BRAKE 100\n");
    Drain();
    unsigned int brake_blink_on = 0U, brake_blink_off = 0U;
    for (unsigned int i = 0U; i < 1600U; ++i) {
        ++ticks;
        WatchdogStep();
        ApplyCommand(NULL);
        assert(hardware_compare == 0U);
        if (appState.link_lost) {
            if (watchdog_led == GPIO_PIN_RESET) { ++brake_blink_on; }
            else { ++brake_blink_off; }
        } else {
            assert(watchdog_led == GPIO_PIN_SET);
        }
    }
    assert(appState.link_lost && appState.brake_active);
    assert(brake_blink_on != 0U && brake_blink_off != 0U);
    Feed("BRAKE 0\n");
    Drain();
    assert(hardware_compare == 0U && !appState.throttle_armed);
    assert(watchdog_led == GPIO_PIN_SET);
    Feed("THROTTLE 40\n");
    Drain();
    assert(hardware_compare == 400U && watchdog_led == GPIO_PIN_SET);
}
static void TestNonblockingTx(void)
{
    Reset();
    Feed("THROTTLE 40\nSTEER -60\n");
    Drain();
    AppState_t state = Snapshot();
    char record[LOG_CAPACITY];
    uint16_t length = FormatRecord(record, &state, NULL, true);
    assert(length < LOG_CAPACITY);
    assert(memcmp(record, "STATUS up_ms=", 13U) == 0);
    assert(TxAppend(record, length));
    unsigned int previous_calls = tx_calls;
    ServiceTx(false);
    assert(tx_calls == previous_calls + 1U && tx_in_flight != 0U);
    uint16_t in_flight = tx_in_flight;
    assert(tx_count == length); /* Keep bytes occupied until completion. */
    ServiceTx(false);
    assert(tx_calls == previous_calls + 1U);
    ServiceTx(true);
    assert(tx_count == length - in_flight);
    while (tx_in_flight != 0U) { ServiceTx(true); }

    Command_t ping = {.type = 'P'};
    length = FormatRecord(record, &state, &ping, false);
    assert(length < LOG_CAPACITY && memcmp(record + length - 4U, "OK\r\n", 4U) == 0);
    while (TxAppend(record, length)) { }
    memcpy(pending_log, record, length);
    pending_log_length = length;
    assert(!FlushRequiredLogs() && pending_log_length == length);
    Feed("BRAKE 100\n"); /* TX fullness never blocks safety assertion/zero. */
    assert(hardware_compare == 0U && appState.pending_positive_brakes != 0U);
    ServiceTx(false);
    ServiceTx(true);
    assert(FlushRequiredLogs() && pending_log_length == 0U);
}
int main(void)
{
    TestParser();
    TestLinesAndLiveness();
    TestOverflowAndErrors();
    TestBrakes();
    TestFullQueue();
    TestWatchdog();
    TestWatchdogLed();
    TestNonblockingTx();
    assert(ack_waits >= 3U);
    puts("PASS: command safety, watchdog boundaries/recovery, blink, retained telemetry");
    return 0;
}
