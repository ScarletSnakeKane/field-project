#include "spiflash.h"

extern SPI_HandleTypeDef hspi1; // SPI1 из CubeMX

#define FLASH_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define FLASH_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

static void SPI_Flash_WaitBusy(void)
{
    uint8_t cmd = FLASH_CMD_RDSR;
    uint8_t status;

    do {
        FLASH_CS_LOW();
        HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
        HAL_SPI_Receive(&hspi1, &status, 1, HAL_MAX_DELAY);
        FLASH_CS_HIGH();
    } while (status & 0x01); // bit0 = BUSY
}

static void SPI_Flash_WriteEnable(void)
{
    uint8_t cmd = FLASH_CMD_WREN;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
    FLASH_CS_HIGH();
}

static void SPI_Flash_GlobalUnprotect(void)
{
    /* WREN */
    SPI_Flash_WriteEnable();

    /* Global Block Protection Unlock */
    uint8_t cmd = FLASH_CMD_GBL_UNPROT;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
    FLASH_CS_HIGH();

    SPI_Flash_WaitBusy();
}

void SPI_Flash_Init(void)
{
    FLASH_CS_HIGH();

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
    HAL_SPI_Transmit(&hspi1, cmd, 4, HAL_MAX_DELAY);
    HAL_SPI_Receive(&hspi1, buf, len, HAL_MAX_DELAY);
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
        HAL_SPI_Transmit(&hspi1, cmd, 4, HAL_MAX_DELAY);
        HAL_SPI_Transmit(&hspi1, (uint8_t*)buf, chunk, HAL_MAX_DELAY);
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
    HAL_SPI_Transmit(&hspi1, cmd, 4, HAL_MAX_DELAY);
    FLASH_CS_HIGH();

    SPI_Flash_WaitBusy();
}
