/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    user_diskio.c
 * @brief   Disk I/O driver for SPI Flash
 ******************************************************************************
 */
 /* USER CODE END Header */

#ifdef USE_OBSOLETE_USER_CODE_SECTION_0
/*
 * Warning: the user section 0 is no more in use (starting from CubeMx version 4.16.0)
 * To be suppressed in the future.
 * Kept to ensure backward compatibility with previous CubeMx versions when
 * migrating projects.
 * User code previously added there should be copied in the new user sections before
 * the section contents can be deleted.
 */
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
#endif

/* USER CODE BEGIN DECL */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "ff_gen_drv.h"
#include "spiflash.h"   // наш драйвер SPI Flash

/* Размеры тома живут в spiflash.h: заявленный объём (FLASH_VIRT_SECTORS) больше
 * физического (FLASH_PHYS_SECTORS) — см. пояснение там. */
#define SECTOR_SIZE     FLASH_SECTOR_BYTES
#define SECTOR_COUNT    FLASH_VIRT_SECTORS


/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* USER CODE END DECL */

/* Private function prototypes -----------------------------------------------*/
DSTATUS USER_initialize (BYTE pdrv);
DSTATUS USER_status (BYTE pdrv);
DRESULT USER_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
#if _USE_WRITE == 1
  DRESULT USER_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT USER_ioctl (BYTE pdrv, BYTE cmd, void *buff);
#endif /* _USE_IOCTL == 1 */

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
    UNUSED(pdrv);
    SPI_Flash_Init();

    /* Раньше здесь безусловно стояло Stat = 0, то есть «диск исправен» — что бы
     * ни творилось на шине SPI. Из-за этого отказ флеш не отличался от пустого
     * тома: FatFs считал носитель готовым, монтирование срывалось на разборе
     * структур, и вышестоящий код видел FR_NO_FILESYSTEM. Дальше срабатывала
     * логика «нет ФС — форматируем», и попытка отформатировать несуществующую
     * микросхему повторялась бы каждый цикл. Теперь спрашиваем саму микросхему. */
    Stat = SPI_Flash_Probe() ? 0 : STA_NOINIT;
    return Stat;
  /* USER CODE END INIT */
}

/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
    UNUSED(pdrv);
    return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
    UNUSED(pdrv);

    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if ((sector + count) > SECTOR_COUNT)
        return RES_PARERR;

    /* Часть заявленного тома физически не существует. Такие сектора отдаём
     * нулями и НИКОГДА не читаем по завёрнутому адресу: SST26 при выходе за
     * границу кристалла вернёт данные с его начала, то есть чужое содержимое. */
    if (sector >= FLASH_PHYS_SECTORS)
    {
        memset(buff, 0, count * SECTOR_SIZE);
        return RES_OK;
    }

    UINT phys = count;
    if ((sector + count) > FLASH_PHYS_SECTORS)
    {
        phys = FLASH_PHYS_SECTORS - sector;
        memset(buff + (phys * SECTOR_SIZE), 0, (count - phys) * SECTOR_SIZE);
    }

    SPI_Flash_Read(sector * SECTOR_SIZE, buff, phys * SECTOR_SIZE);
    return RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
    UNUSED(pdrv);

    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if ((sector + count) > SECTOR_COUNT)
        return RES_PARERR;

    /* Запись за физическую границу — это молчаливая потеря данных: байты уйдут
     * в никуда, а вызывающий решит, что всё записано. Ограничение CSV_MAX_BYTES
     * в main.c не даёт сюда дойти; если всё же дошли — честная ошибка. */
    if ((sector + count) > FLASH_PHYS_SECTORS)
        return RES_ERROR;

    uint32_t addr = sector * SECTOR_SIZE;
    uint32_t len  = count * SECTOR_SIZE;

    static uint8_t block_buf[FLASH_SECTOR_SIZE]; // static! не на стеке

    uint32_t block_start = addr & ~(FLASH_SECTOR_SIZE - 1);
    uint32_t block_end   = (addr + len + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);

    for (uint32_t blk = block_start; blk < block_end; blk += FLASH_SECTOR_SIZE)
    {
        /* 1) читаем существующий блок целиком */
        SPI_Flash_Read(blk, block_buf, FLASH_SECTOR_SIZE);

        /* 2) вычисляем пересечение [addr, addr+len) с этим блоком */
        uint32_t copy_start = (addr > blk) ? addr : blk;
        uint32_t copy_end   = (addr + len < blk + FLASH_SECTOR_SIZE) ? (addr + len) : (blk + FLASH_SECTOR_SIZE);

        if (copy_start < copy_end)
        {
            uint32_t off_in_block = copy_start - blk;
            uint32_t off_in_src   = copy_start - addr;
            uint32_t copy_len     = copy_end - copy_start;
            memcpy(&block_buf[off_in_block], &buff[off_in_src], copy_len);
        }

        /* 3) стираем и пишем блок целиком */
        SPI_Flash_EraseSector(blk);
        SPI_Flash_Write(blk, block_buf, FLASH_SECTOR_SIZE);
    }

    return RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
    UNUSED(pdrv);

    switch (cmd)
    {
        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_COUNT:
            *(DWORD*)buff = SECTOR_COUNT;
            return RES_OK;

        case GET_SECTOR_SIZE:
            *(WORD*)buff = SECTOR_SIZE;
            return RES_OK;

        case GET_BLOCK_SIZE:
            *(DWORD*)buff = FLASH_SECTOR_SIZE / SECTOR_SIZE;
            return RES_OK;
    }

    return RES_PARERR;
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */

