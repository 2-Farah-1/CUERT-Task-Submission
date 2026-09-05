#include "usb_uart_bridge.h"
#include "main.h"
#include "usbd_cdc_if.h"
#if USB_BRIDGE_SELF_TEST
#include <string.h>
#endif

#define USB_QUEUE_SIZE 256U
#define UART_QUEUE_SIZE 1024U
#define PACKET_SIZE CDC_DATA_FS_MAX_PACKET_SIZE

extern UART_HandleTypeDef huart1;
extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t usb_queue[USB_QUEUE_SIZE];
static uint8_t uart_queue[UART_QUEUE_SIZE];
#if !USB_BRIDGE_SELF_TEST
static uint8_t uart_tx[PACKET_SIZE];
static uint8_t uart_rx;
#endif
static volatile uint16_t usb_head, usb_tail, uart_head, uart_tail;
static volatile uint8_t usb_online, usb_rx_paused;
static uint16_t usb_inflight;

volatile uint32_t bridge_uart_rx_dropped;
volatile uint32_t bridge_uart_errors;

#if USB_BRIDGE_SELF_TEST
#define LED_PINS (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2)
static char command[6];
static uint8_t command_length, command_overflow;

/* Called once per main-loop poll with interrupts masked, like the queues.
   Consume at most one byte; reserve room for the longest response first. */
static void SelfTest_Poll(void)
{
  uint16_t free_bytes = (uart_tail + UART_QUEUE_SIZE - uart_head - 1U) % UART_QUEUE_SIZE;
  if (usb_tail == usb_head || free_bytes < sizeof("OK MIDDLE\r\n") - 1U)
  {
    return;
  }

  uint8_t byte = usb_queue[usb_tail];
  usb_tail = (usb_tail + 1U) % USB_QUEUE_SIZE;
  if (byte != '\r' && byte != '\n')
  {
    if (command_length < sizeof(command))
    {
      command[command_length++] = (char)byte;
    }
    else
    {
      command_overflow = 1;
    }
    return;
  }

  /* Ignore empty lines, including the LF half of CRLF across USB packets. */
  if (command_length == 0 && !command_overflow) return;

  const char *response = "ERR\r\n";
  uint16_t led = 0;
  uint8_t recognized = 0;
  if (!command_overflow)
  {
    if (command_length == 4 && memcmp(command, "LEFT", 4) == 0)
    {
      led = GPIO_PIN_0;
      response = "OK LEFT\r\n";
      recognized = 1;
    }
    else if (command_length == 6 && memcmp(command, "MIDDLE", 6) == 0)
    {
      led = GPIO_PIN_1;
      response = "OK MIDDLE\r\n";
      recognized = 1;
    }
    else if (command_length == 5 && memcmp(command, "RIGHT", 5) == 0)
    {
      led = GPIO_PIN_2;
      response = "OK RIGHT\r\n";
      recognized = 1;
    }
    else if (command_length == 3 && memcmp(command, "OFF", 3) == 0)
    {
      response = "OK OFF\r\n";
      recognized = 1;
    }
  }
  if (recognized)
  {
    HAL_GPIO_WritePin(GPIOA, LED_PINS, GPIO_PIN_RESET);
    if (led != 0) HAL_GPIO_WritePin(GPIOA, led, GPIO_PIN_SET);
  }
  for (const char *p = response; *p != '\0'; ++p)
  {
    uart_queue[uart_head] = (uint8_t)*p;
    uart_head = (uart_head + 1U) % UART_QUEUE_SIZE;
  }
  command_length = command_overflow = 0;
}
#endif

void Bridge_Init(void)
{
#if USB_BRIDGE_SELF_TEST
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, LED_PINS, GPIO_PIN_RESET);
  gpio.Pin = LED_PINS;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);
#else
  (void)HAL_UART_Receive_IT(&huart1, &uart_rx, 1);
#endif
}

void Bridge_UsbInit(void)
{
  /* A new USB session discards queued OUT data, but not active UART TX. */
  usb_head = usb_tail = 0;
#if USB_BRIDGE_SELF_TEST
  command_length = command_overflow = 0;
#endif
  usb_inflight = 0;
  usb_rx_paused = 0;
  usb_online = 1;
}

void Bridge_UsbDeInit(void)
{
  usb_online = 0;
  /* Keep unconfirmed IN bytes for retry after reconfiguration. */
  usb_inflight = 0;
}

void Bridge_UsbReceive(const uint8_t *data, uint32_t length)
{
  /* One packet always fits: Poll rearms OUT only with a full packet free. */
  for (uint32_t i = 0; i < length; ++i)
  {
    usb_queue[usb_head] = data[i];
    usb_head = (usb_head + 1U) % USB_QUEUE_SIZE;
  }
  usb_rx_paused = 1;
}

void Bridge_Poll(void)
{
  /* Serialize short queue/HAL operations with USB reset and UART callbacks.
     No waits: each copy is limited to one USB packet. */
  uint32_t mask = __get_PRIMASK();
  __disable_irq();

#if USB_BRIDGE_SELF_TEST
  SelfTest_Poll();
#else
  if (huart1.RxState == HAL_UART_STATE_READY)
  {
    (void)HAL_UART_Receive_IT(&huart1, &uart_rx, 1);
  }

  if (huart1.gState == HAL_UART_STATE_READY && usb_tail != usb_head)
  {
    uint16_t tail = usb_tail;
    uint16_t count = 0;
    while (tail != usb_head && count < PACKET_SIZE)
    {
      uart_tx[count++] = usb_queue[tail];
      tail = (tail + 1U) % USB_QUEUE_SIZE;
    }
    if (HAL_UART_Transmit_IT(&huart1, uart_tx, count) == HAL_OK)
    {
      usb_tail = tail;
    }
  }
#endif

  USBD_CDC_HandleTypeDef *cdc = hUsbDeviceFS.pClassData;
  if (usb_online && hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED && cdc != NULL)
  {
    uint16_t free_bytes = (usb_tail + USB_QUEUE_SIZE - usb_head - 1U) % USB_QUEUE_SIZE;
    if (usb_rx_paused && free_bytes >= PACKET_SIZE)
    {
      if (USBD_CDC_ReceivePacket(&hUsbDeviceFS) == USBD_OK)
      {
        usb_rx_paused = 0;
      }
    }

    /* TxState reaches zero only after data and any terminating ZLP complete.
       The ring storage remains owned by USB until then. */
    if (usb_inflight != 0 && cdc->TxState == 0)
    {
      uart_tail = (uart_tail + usb_inflight) % UART_QUEUE_SIZE;
      usb_inflight = 0;
    }
    if (usb_inflight == 0 && uart_tail != uart_head)
    {
      uint16_t count = (uart_head > uart_tail) ?
          uart_head - uart_tail : UART_QUEUE_SIZE - uart_tail;
      if (count > PACKET_SIZE) count = PACKET_SIZE;
      if (CDC_Transmit_FS(&uart_queue[uart_tail], count) == USBD_OK)
      {
        usb_inflight = count;
      }
      /* BUSY/FAIL leaves all bytes queued for the next poll. */
    }
  }
  __set_PRIMASK(mask);
}

#if !USB_BRIDGE_SELF_TEST
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
  if (uart != &huart1) return;
  uint16_t next = (uart_head + 1U) % UART_QUEUE_SIZE;
  if (next != uart_tail)
  {
    uart_queue[uart_head] = uart_rx;
    uart_head = next;
  }
  else
  {
    ++bridge_uart_rx_dropped;
  }
  /* Leave error handling to ErrorCallback: rearming here would clear the
     HAL ErrorCode before its IRQ handler checks for overrun. */
  if (uart->ErrorCode == HAL_UART_ERROR_NONE)
  {
    (void)HAL_UART_Receive_IT(uart, &uart_rx, 1);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
  if (uart != &huart1) return;
  ++bridge_uart_errors;
  /* No DMA: AbortReceive only resets RX state/interrupts and does not wait.
     TX continues independently. SR/DR read clears PE/FE/NE/ORE. */
  (void)HAL_UART_AbortReceive(uart);
  __HAL_UART_CLEAR_OREFLAG(uart);
  (void)HAL_UART_Receive_IT(uart, &uart_rx, 1);
}
#endif
