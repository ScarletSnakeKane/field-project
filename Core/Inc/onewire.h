#ifndef __ONEWIRE_H__
#define __ONEWIRE_H__

#include "stm32f4xx_hal.h"

void OneWire_Delay(uint32_t us);

uint8_t OneWire_Reset(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);
void    OneWire_WriteBit(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, uint8_t bit);
uint8_t OneWire_ReadBit(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);

void    OneWire_WriteByte(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, uint8_t byte);
uint8_t OneWire_ReadByte(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin);

#endif
