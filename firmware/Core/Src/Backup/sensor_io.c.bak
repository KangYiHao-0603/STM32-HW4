/**
 * sensor_io.c
 * SENSOR_IO_xxx for the LSM6DSL driver (Drivers/BSP/Components/lsm6dsl) on B-L475E-IOT01A.
 * Replaces the I2C part of stm32l475e_iot01.c, which cannot be in this project
 * (its BSP_LED/PB/COM collide with custom.c).
 *
 *   I2C2: PB10 = SCL, PB11 = SDA (AF4), 100 kHz
 *
 * HAL_I2C_MODULE_ENABLED is off in stm32l4xx_hal_conf.h (I2C2 is not configured in the .ioc),
 * so this file drives the I2C2 registers directly. Kernel clock = HSI16, so the timing below
 * stays valid whatever SYSCLK/PCLK1 is.
 *
 * Only TASK_ACC uses these functions, so no locking is done here.
 */
#include "main.h"

#define I2C_TIMEOUT_MS        10u
/* 100 kHz @ 16 MHz (RM0351 timing example): PRESC=3, SCLDEL=4, SDADEL=2, SCLH=0x0F, SCLL=0x13 */
#define I2C_TIMING_100K_16M   0x30420F13u

static int i2c_wait_flag(uint32_t flag)
{
  uint32_t start = HAL_GetTick();
  while ((I2C2->ISR & flag) == 0u)
  {
    if (I2C2->ISR & I2C_ISR_NACKF)
    {
      I2C2->ICR = I2C_ICR_NACKCF;
      /* NACK ends the transfer with STOP (AUTOEND) or we generate it */
      if ((I2C2->CR2 & I2C_CR2_AUTOEND) == 0u) I2C2->CR2 |= I2C_CR2_STOP;
      while ((I2C2->ISR & I2C_ISR_STOPF) == 0u && (HAL_GetTick() - start) < I2C_TIMEOUT_MS) {}
      I2C2->ICR = I2C_ICR_STOPCF;
      return -1;
    }
    if ((HAL_GetTick() - start) > I2C_TIMEOUT_MS)
    {
      return -1;
    }
  }
  return 0;
}

static void i2c_start(uint8_t addr, uint32_t nbytes, uint32_t extra)
{
  I2C2->CR2 = ((uint32_t)addr & I2C_CR2_SADD) | (nbytes << I2C_CR2_NBYTES_Pos) | extra | I2C_CR2_START;
}

static int i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint16_t len)
{
  if (len > 254u) return -1;
  i2c_start(addr, (uint32_t)len + 1u, I2C_CR2_AUTOEND);

  if (i2c_wait_flag(I2C_ISR_TXIS)) return -1;
  I2C2->TXDR = reg;
  for (uint16_t i = 0; i < len; i++)
  {
    if (i2c_wait_flag(I2C_ISR_TXIS)) return -1;
    I2C2->TXDR = buf[i];
  }
  if (i2c_wait_flag(I2C_ISR_STOPF)) return -1;
  I2C2->ICR = I2C_ICR_STOPCF;
  return 0;
}

static int i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len)
{
  if (len == 0u || len > 255u) return -1;

  /* register address, no STOP (software end) */
  i2c_start(addr, 1u, 0u);
  if (i2c_wait_flag(I2C_ISR_TXIS)) return -1;
  I2C2->TXDR = reg;
  if (i2c_wait_flag(I2C_ISR_TC)) return -1;

  /* repeated START, read len bytes, then STOP */
  i2c_start(addr, len, I2C_CR2_RD_WRN | I2C_CR2_AUTOEND);
  for (uint16_t i = 0; i < len; i++)
  {
    if (i2c_wait_flag(I2C_ISR_RXNE)) return -1;
    buf[i] = (uint8_t)I2C2->RXDR;
  }
  if (i2c_wait_flag(I2C_ISR_STOPF)) return -1;
  I2C2->ICR = I2C_ICR_STOPCF;
  return 0;
}

/* ================================================================ */
void SENSOR_IO_Init(void)
{
  static uint8_t initialized = 0;
  GPIO_InitTypeDef gpio = {0};

  if (initialized) return;
  initialized = 1;

  /* HSI16 as I2C2 kernel clock */
  RCC->CR |= RCC_CR_HSION;
  while ((RCC->CR & RCC_CR_HSIRDY) == 0u) {}
  MODIFY_REG(RCC->CCIPR, RCC_CCIPR_I2C2SEL, RCC_CCIPR_I2C2SEL_1);   /* 10b = HSI16 */

  __HAL_RCC_GPIOB_CLK_ENABLE();
  gpio.Pin       = GPIO_PIN_10 | GPIO_PIN_11;
  gpio.Mode      = GPIO_MODE_AF_OD;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF4_I2C2;
  HAL_GPIO_Init(GPIOB, &gpio);

  __HAL_RCC_I2C2_CLK_ENABLE();
  __HAL_RCC_I2C2_FORCE_RESET();
  __HAL_RCC_I2C2_RELEASE_RESET();

  I2C2->CR1     = 0u;                       /* PE = 0 while configuring */
  I2C2->TIMINGR = I2C_TIMING_100K_16M;
  I2C2->OAR1    = 0u;
  I2C2->OAR2    = 0u;
  I2C2->CR2     = 0u;                       /* 7-bit addressing */
  I2C2->CR1     = I2C_CR1_PE;
}

void SENSOR_IO_Write(uint8_t Addr, uint8_t Reg, uint8_t Value)
{
  (void)i2c_write(Addr, Reg, &Value, 1u);
}

uint8_t SENSOR_IO_Read(uint8_t Addr, uint8_t Reg)
{
  uint8_t value = 0;
  (void)i2c_read(Addr, Reg, &value, 1u);
  return value;
}

uint16_t SENSOR_IO_ReadMultiple(uint8_t Addr, uint8_t Reg, uint8_t *Buffer, uint16_t Length)
{
  return (i2c_read(Addr, Reg, Buffer, Length) == 0) ? 0u : 1u;
}

void SENSOR_IO_WriteMultiple(uint8_t Addr, uint8_t Reg, uint8_t *Buffer, uint16_t Length)
{
  (void)i2c_write(Addr, Reg, Buffer, Length);
}
