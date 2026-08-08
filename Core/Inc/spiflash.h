#ifndef SPIFLASH_H
#define SPIFLASH_H

#include "stm32f4xx_hal.h"

/* SST26VF016B: 16 Mbit = 2 MByte */
#define FLASH_PAGE_SIZE       256        // байт
#define FLASH_SECTOR_SIZE     4096       // байт (4 KB)
#define FLASH_SIZE_BYTES      (2 * 1024 * 1024) // 2 MB

/* Команды SST26VF016B */
#define FLASH_CMD_READ        0x03       // Read Data
#define FLASH_CMD_PP          0x02       // Page Program
#define FLASH_CMD_WREN        0x06       // Write Enable
#define FLASH_CMD_RDSR        0x05       // Read Status Register
#define FLASH_CMD_SE_4K       0x20       // 4KB Sector Erase
#define FLASH_CMD_GBL_UNPROT  0x98       // Global Block Protection Unlock
#define FLASH_CMD_RDID        0x9F       // Read JEDEC ID
#define FLASH_CMD_DPD         0xB9       // Deep Power-Down
#define FLASH_CMD_RDPD        0xAB       // Release from Deep Power-Down

void SPI_Flash_Init(void);
void SPI_Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void SPI_Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
void SPI_Flash_EraseSector(uint32_t addr);

/* Deep Power-Down: флеш запитана постоянно (отдельного ключа на плате нет),
 * поэтому в простое её ток снижается только этими командами. */
void SPI_Flash_DeepPowerDown(void);
void SPI_Flash_ReleasePowerDown(void);

#endif
