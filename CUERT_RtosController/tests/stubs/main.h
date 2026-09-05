#ifndef TEST_MAIN_H
#define TEST_MAIN_H
#include <stdint.h>
typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY } HAL_StatusTypeDef;
#define HAL_UART_ERROR_NONE 0U
#define HAL_UART_ERROR_ORE 8U
#define HAL_UART_STATE_READY 0U
#define HAL_UART_STATE_BUSY_RX 1U
#define TIM_CHANNEL_1 0U
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
#define GPIOC ((void *)8)
#define GPIO_PIN_13 (1U << 13)
void HAL_GPIO_WritePin(void *port, uint16_t pin, GPIO_PinState state);
typedef struct {
    uint32_t ErrorCode;
    uint32_t RxState;
} UART_HandleTypeDef;
typedef struct {
    struct { uint32_t Period; } Init;
} TIM_HandleTypeDef;
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t mask);
void TestSetCompare(uint32_t compare);
#define __HAL_TIM_SET_COMPARE(timer, channel, compare) TestSetCompare(compare)
#define __HAL_TIM_DISABLE_OCxPRELOAD(timer, channel) ((void)0)
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *uart, uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *timer, uint32_t channel);
void Error_Handler(void);
#endif
