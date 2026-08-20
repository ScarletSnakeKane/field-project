#ifndef SPIFLASH_H
#define SPIFLASH_H

#include "stm32f4xx_hal.h"

/* SST26VF016B: 16 Mbit = 2 MByte */
#define FLASH_PAGE_SIZE       256        // байт
#define FLASH_SECTOR_SIZE     4096       // байт (4 KB) — сектор стирания
#define FLASH_SIZE_BYTES      (2 * 1024 * 1024) // 2 MB, физический объём

/* Логический сектор тома FAT (не путать с сектором стирания флеш выше). */
#define FLASH_SECTOR_BYTES    512
#define FLASH_PHYS_SECTORS    (FLASH_SIZE_BYTES / FLASH_SECTOR_BYTES)   /* 4096 — сколько есть на самом деле */

/* Хосту заявляется вдвое больший том, и это сделано намеренно.
 *
 * На 2 МБ при 512-байтном кластере получается 3984 кластера, а FAT16 начинается
 * с 4085 — том неизбежно форматируется как FAT12. Windows его читает, Android
 * монтировать отказывается («Unsupported USB drive»). Увеличить число кластеров
 * на реальном объёме нельзя: кластер уже минимальный.
 *
 * Поэтому том объявляется размером 4 МБ: кластеров становится ~8100, тип тома —
 * FAT16, и телефон его принимает. Секторов выше FLASH_PHYS_SECTORS физически
 * не существует: при чтении они отдаются нулями, при записи возвращается ошибка.
 * Файл растёт от начала диска, поэтому до несуществующей области он доберётся
 * очень нескоро — и не доберётся вовсе, пока действует CSV_MAX_BYTES в main.c. */
#define FLASH_VIRT_SECTORS    8192      /* 4 МБ — заявленный объём */

/* Таймауты. Устройство стоит в поле без присмотра — любое ожидание должно
 * быть конечным, иначе единственный сбой связи вешает прошивку навсегда. */
#define FLASH_SPI_TIMEOUT_MS  100U       // отдельная SPI-транзакция
#define FLASH_BUSY_TIMEOUT_MS 500U       // ожидание конца записи/стирания (стирание сектора ~25-50 мс)

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
/* Отвечает ли микросхема вообще. Возвращает 0, если JEDEC ID вычитать не вышло:
 * на неподключённой или мёртвой шине ответ вырождается в сплошные 0x00 или 0xFF. */
uint8_t SPI_Flash_Probe(void);
void SPI_Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void SPI_Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
void SPI_Flash_EraseSector(uint32_t addr);

/* Deep Power-Down: флеш запитана постоянно (отдельного ключа на плате нет),
 * поэтому в простое её ток снижается только этими командами. */
void SPI_Flash_DeepPowerDown(void);
void SPI_Flash_ReleasePowerDown(void);

#endif
