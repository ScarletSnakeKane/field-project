#ifndef SPIFLASH_H
#define SPIFLASH_H

#include "stm32f4xx_hal.h"
/* Правила «когда можно писать и отдавать флеш хосту» — рядом с драйвером,
 * чтобы каждый его пользователь видел их без отдельного include. */
#include "storage_policy.h"
/* Архив на флеш — кольцевой журнал (П5а). Реализация включена в spiflash.c;
 * журнал работает с флеш через SPI_Flash_Read/Write/EraseSector. */
#include "log_ring.h"

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
 * С П5а FAT на флеш больше нет (её занимает журнал), а размер — то, что USB
 * пока отдаёт хосту сырыми секторами; виртуальный том для USB — П5б. */
#define FLASH_VIRT_SECTORS    8192      /* 4 МБ — заявленный объём */

/* Таймауты. Устройство стоит в поле без присмотра — любое ожидание должно
 * быть конечным, иначе единственный сбой связи вешает прошивку навсегда. */
#define FLASH_SPI_TIMEOUT_MS  100U       // отдельная SPI-транзакция
#define FLASH_BUSY_TIMEOUT_MS 500U       // ожидание конца записи/стирания (сектор SST26 — до 25 мс; запас на совместимые)

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
#define FLASH_CMD_RBPR        0x72       // Read Block-Protection Register (SST26)
#define FLASH_BPR_LEN         6U         // байт BPR у SST26VF016B
#define FLASH_MFR_SST         0xBFU      // JEDEC: SST/Microchip

void SPI_Flash_Init(void);
/* Снять защиту блоков (WREN + 0x98) и проверить: у SST26 — чтением регистра
 * защиты (все биты должны быть 0), у совместимых без него — по успеху
 * команд. SST26 после собственного сброса по питанию (просадка, которую МК
 * пережил) запирает все блоки, и программирование со стиранием молча не
 * делают ничего; поэтому журнал зовёт это перед каждой серией записи (T2).
 * 1 — защита снята. */
uint8_t SPI_Flash_Unlock(void);
/* Отвечает ли микросхема вообще. Возвращает 0, если JEDEC ID вычитать не вышло:
 * на неподключённой или мёртвой шине ответ вырождается в сплошные 0x00 или 0xFF. */
uint8_t SPI_Flash_Probe(void);
/* Чтение, запись и стирание возвращают 1 при успехе и 0 при отказе: ошибка HAL
 * или таймаут BUSY. Раньше они были void, и мусор с шины уходил в FatFs как
 * валидные данные — а по такому чтению старт решал отформатировать архив. */
uint8_t SPI_Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len);
uint8_t SPI_Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
uint8_t SPI_Flash_EraseSector(uint32_t addr);
/* Прочитать обратно и сравнить с buf: программирование на просевшем питании
 * или в деградирующий чип может «пройти» без единой ошибки шины. */
uint8_t SPI_Flash_Verify(uint32_t addr, const uint8_t *buf, uint32_t len);

/* Разделение флеш между основным потоком и MSC в прерывании OTG.
 * SetBusy(1) основной поток ставит на всю серию операций FatFs, SetBusy(0) —
 * по её окончании. IsBusy() истинно и во время любой отдельной операции
 * драйвера. IsReady() — микросхема ответила на Probe и не спит в DPD.
 * MSC обязан спрашивать оба признака и при отказе отвечать хосту «не готов»:
 * из прерывания флеш не инициализировать и не ждать. */
void    SPI_Flash_SetBusy(uint8_t busy);
uint8_t SPI_Flash_IsBusy(void);
uint8_t SPI_Flash_IsReady(void);

/* MSC отмечает начало USB-сессии, основной поток забирает отметку и
 * перемонтирует том, прежде чем снова писать. */
void    SPI_Flash_NoteHostSession(void);
uint8_t SPI_Flash_TakeHostSession(void);

/* Deep Power-Down: флеш запитана постоянно (отдельного ключа на плате нет),
 * поэтому в простое её ток снижается только этими командами. */
void SPI_Flash_DeepPowerDown(void);
void SPI_Flash_ReleasePowerDown(void);

#endif
