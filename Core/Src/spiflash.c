#include "spiflash.h"

extern SPI_HandleTypeDef hspi1; // SPI1 из CubeMX

#define FLASH_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define FLASH_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

/* Возвращает 1, если флеш освободилась, 0 — если истёк таймаут.
 * Раньше здесь стоял HAL_MAX_DELAY и бесконечный цикл: одна сорванная транзакция
 * или чип, не вышедший из Deep Power-Down, вешали устройство навсегда. */
static uint8_t SPI_Flash_WaitBusy(void)
{
    uint8_t cmd = FLASH_CMD_RDSR;
    uint8_t status;
    uint32_t start = HAL_GetTick();

    do {
        FLASH_CS_LOW();
        HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
        HAL_SPI_Receive(&hspi1, &status, 1, FLASH_SPI_TIMEOUT_MS);
        FLASH_CS_HIGH();

        if ((status & 0x01) == 0)   // bit0 = BUSY
            return 1;

    } while ((HAL_GetTick() - start) < FLASH_BUSY_TIMEOUT_MS);

    return 0;   // не дождались — операция считается неудачной, но управление возвращаем
}

static void SPI_Flash_WriteEnable(void)
{
    uint8_t cmd = FLASH_CMD_WREN;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
}

static void SPI_Flash_GlobalUnprotect(void)
{
    /* WREN */
    SPI_Flash_WriteEnable();

    /* Global Block Protection Unlock */
    uint8_t cmd = FLASH_CMD_GBL_UNPROT;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();

    SPI_Flash_WaitBusy();
}

void SPI_Flash_DeepPowerDown(void)
{
    /* Нельзя обрывать незавершённое стирание/запись переходом в DPD —
     * сектор может остаться повреждённым. Дожидаемся завершения. */
    SPI_Flash_WaitBusy();

    uint8_t cmd = FLASH_CMD_DPD;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
}

void SPI_Flash_ReleasePowerDown(void)
{
    uint8_t cmd = FLASH_CMD_RDPD;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();

    /* Выход из DPD занимает единицы-десятки мкс; 1 мс с большим запасом —
     * это происходит раз в цикл сна, экономить тут нечего. */
    HAL_Delay(1);
}

void SPI_Flash_Init(void)
{
    FLASH_CS_HIGH();

    /* Флеш сидит на постоянном питании, поэтому Deep Power-Down переживает сброс МК:
     * если ресет/перепрошивка случились во время сна, чип всё ещё в DPD и игнорирует
     * любые команды, кроме 0xAB. Будим его до первого обращения — иначе
     * SPI_Flash_WaitBusy() ниже будет впустую ждать до самого таймаута. */
    SPI_Flash_ReleasePowerDown();

    /* Снять защиту блоков, иначе запись/стирание будут игнорироваться */
    SPI_Flash_GlobalUnprotect();
}

void SPI_Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4];

    cmd[0] = FLASH_CMD_READ;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8)  & 0xFF;
    cmd[3] = addr & 0xFF;

    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS);
    HAL_SPI_Receive(&hspi1, buf, len, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
}

void SPI_Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    while (len > 0)
    {
        uint32_t page_off = addr % FLASH_PAGE_SIZE;
        uint32_t chunk    = FLASH_PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;

        SPI_Flash_WriteEnable();

        uint8_t cmd[4];
        cmd[0] = FLASH_CMD_PP;
        cmd[1] = (addr >> 16) & 0xFF;
        cmd[2] = (addr >> 8)  & 0xFF;
        cmd[3] = addr & 0xFF;

        FLASH_CS_LOW();
        HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS);
        HAL_SPI_Transmit(&hspi1, (uint8_t*)buf, chunk, FLASH_SPI_TIMEOUT_MS);
        FLASH_CS_HIGH();

        SPI_Flash_WaitBusy();

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
}

void SPI_Flash_EraseSector(uint32_t addr)
{
    /* выровнять по 4KB */
    addr &= ~(FLASH_SECTOR_SIZE - 1);

    SPI_Flash_WriteEnable();

    uint8_t cmd[4];
    cmd[0] = FLASH_CMD_SE_4K;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8)  & 0xFF;
    cmd[3] = addr & 0xFF;

    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();

    SPI_Flash_WaitBusy();
}
