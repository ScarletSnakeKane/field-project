/* Реализация кольцевого журнала (single-header, log_ring.h) — здесь, в
 * существующем .c: новый файл сборка CubeIDE не увидела бы (Debug/ не в git).
 * Макрос — до первого включения: spiflash.h сам включает log_ring.h. */
#define LOG_RING_IMPLEMENTATION
#include "spiflash.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1; // SPI1 из CubeMX

#define FLASH_CS_LOW()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET)
#define FLASH_CS_HIGH()  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET)

/* Флеш делят два контекста: основной поток (FatFs) и прерывание OTG (MSC читает
 * сырые сектора для хоста). Взаимного исключения на уровне HAL нет: прерывание,
 * вклинившись посреди кадра основного потока, подняло бы CS и оборвало Page
 * Program или чтение блока перед RMW. Поэтому основной поток объявляет флеш
 * занятой, а MSC при этом отвечает хосту «не готов» и тот повторяет запрос.
 *   session_busy — выставляет main на всю серию операций FatFs (том должен
 *                  быть согласован, а не только отдельный кадр SPI);
 *   op_depth     — любая операция этого драйвера, даже вне серии. Счётчик,
 *                  а не флаг: операции вложены (Init зовёт ReleasePowerDown). */
static volatile uint8_t session_busy = 0;
static volatile uint8_t op_depth     = 0;
static volatile uint8_t probed_ok    = 0;   /* Probe ответил живым ID */
static volatile uint8_t in_dpd       = 1;   /* после сброса МК чип может спать */
static volatile uint8_t host_session = 0;   /* был ли хост с момента прошлой проверки */
static uint8_t          is_sst26     = 0;   /* RDID ответил BF 26 xx (запоминается только «да») */

#define OP_BEGIN()  (op_depth++)
#define OP_END()    (op_depth--)

/* Задержка без SysTick. Драйвер вызывается и из прерывания OTG с приоритетом 0,
 * где SysTick (приоритет 15) не тикает: HAL_Delay там не вернётся никогда,
 * а таймаут по HAL_GetTick не истечёт. Цикл ниже занимает не меньше 4 тактов
 * на итерацию (volatile-счётчик плюс ожидания флеш-памяти), поэтому реальная
 * задержка выходит не короче заказанной — для таймаутов это безопасная сторона. */
static void SPI_Flash_DelayUs(uint32_t us)
{
    volatile uint32_t n = (SystemCoreClock / 4000000U) * us;
    while (n > 0U)
        n--;
}

/* Пауза между опросами BUSY: ограничивает число опросов при длинном стирании
 * и задаёт таймаут счётом, а не по часам. */
#define FLASH_POLL_US   20U

/* Возвращает 1, если флеш освободилась, 0 — если истёк таймаут или шина отказала.
 * Раньше здесь стоял HAL_MAX_DELAY и бесконечный цикл: одна сорванная транзакция
 * или чип, не вышедший из Deep Power-Down, вешали устройство навсегда. */
static uint8_t SPI_Flash_WaitBusy(void)
{
    uint8_t cmd = FLASH_CMD_RDSR;
    uint8_t status;
    uint32_t polls = (FLASH_BUSY_TIMEOUT_MS * 1000U) / FLASH_POLL_US;
    /* Срок — по счётчику тактов (S5). Один опрос — это не 20 мкс, а две
     * HAL-транзакции плюс пауза, 60–100 мкс, и по одному счёту опросов
     * мёртвая флеш (MISO в 1, BUSY висит) держала WaitBusy 1.5–2.5 с вместо
     * заявленных 0.5 — на старте до шести таких подряд без пинка сторожа.
     * DWT->CYCCNT запущен в main до всего остального и идёт и в прерывании,
     * где SysTick стоит. Счёт опросов остаётся верхней границей — на случай,
     * если счётчик тактов кто-то остановит. */
    uint32_t t0    = DWT->CYCCNT;
    uint32_t limit = (SystemCoreClock / 1000U) * FLASH_BUSY_TIMEOUT_MS;

    while (polls-- > 0U && (DWT->CYCCNT - t0) < limit)
    {
        status = 0xFF;
        FLASH_CS_LOW();
        HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
        if (st == HAL_OK)
            st = HAL_SPI_Receive(&hspi1, &status, 1, FLASH_SPI_TIMEOUT_MS);
        FLASH_CS_HIGH();

        /* Сбой SPI — не «свободна»: статус не прочитан, верить ему нельзя. */
        if (st == HAL_OK && (status & 0x01) == 0)   // bit0 = BUSY
            return 1;

        SPI_Flash_DelayUs(FLASH_POLL_US);
    }

    return 0;   // не дождались — операция считается неудачной, но управление возвращаем
}

static uint8_t SPI_Flash_WriteEnable(void)
{
    uint8_t cmd = FLASH_CMD_WREN;
    FLASH_CS_LOW();
    HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
    return (st == HAL_OK) ? 1U : 0U;
}

/* Прочитать len байт ответа на однобайтовую команду. */
static uint8_t SPI_Flash_ReadReg(uint8_t cmd, uint8_t *buf, uint32_t len)
{
    FLASH_CS_LOW();
    HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    if (st == HAL_OK)
        st = HAL_SPI_Receive(&hspi1, buf, len, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
    return (st == HAL_OK) ? 1U : 0U;
}

/* Раньше результат не проверялся вовсе: сорванный WREN или 0x98 оставлял
 * блоки запертыми, и запись молча не шла до перезагрузки МК (T2). */
static uint8_t SPI_Flash_GlobalUnprotect(void)
{
    /* WREN, поданный занятому чипу, игнорируется — сначала дождаться. */
    uint8_t ok = SPI_Flash_WaitBusy() && SPI_Flash_WriteEnable();

    /* Global Block Protection Unlock */
    if (ok)
    {
        uint8_t cmd = FLASH_CMD_GBL_UNPROT;
        FLASH_CS_LOW();
        ok = (HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS) == HAL_OK);
        FLASH_CS_HIGH();
    }
    if (ok)
        ok = SPI_Flash_WaitBusy();

    /* Проверка по регистру защиты — только у SST26: у совместимых чипов
     * команды 0x72 может не быть, и сверка дала бы ложный отказ. Им
     * остаётся сверка каждой записи чтением в журнале. */
    /* Запоминается только опознанный SST26: искажённое первое чтение ID
     * иначе навсегда отключило бы проверку; не опознан — спросим снова. */
    if (ok && !is_sst26)
    {
        uint8_t id[3] = {0};
        if (SPI_Flash_ReadReg(FLASH_CMD_RDID, id, sizeof(id)) &&
            id[0] == FLASH_MFR_SST && id[1] == FLASH_TYPE_SST26)
            is_sst26 = 1;
    }
    if (ok && is_sst26)
    {
        uint8_t bpr[FLASH_BPR_LEN];
        ok = SPI_Flash_ReadReg(FLASH_CMD_RBPR, bpr, sizeof(bpr));
        for (uint32_t i = 0; ok && i < sizeof(bpr); i++)
            if (bpr[i] != 0U)
                ok = 0;
    }
    return ok;
}

uint8_t SPI_Flash_Unlock(void)
{
    OP_BEGIN();
    uint8_t ok = SPI_Flash_GlobalUnprotect();
    OP_END();
    return ok;
}

void SPI_Flash_DeepPowerDown(void)
{
    OP_BEGIN();
    /* Нельзя обрывать незавершённое стирание/запись переходом в DPD —
     * сектор может остаться повреждённым. Дожидаемся завершения. */
    SPI_Flash_WaitBusy();

    uint8_t cmd = FLASH_CMD_DPD;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
    in_dpd = 1;
    OP_END();
}

void SPI_Flash_ReleasePowerDown(void)
{
    OP_BEGIN();
    uint8_t cmd = FLASH_CMD_RDPD;
    FLASH_CS_LOW();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();

    /* Выход из DPD занимает единицы-десятки мкс; 1 мс с большим запасом —
     * это происходит раз в цикл сна, экономить тут нечего. */
    SPI_Flash_DelayUs(1000U);
    in_dpd = 0;
    OP_END();
}

void SPI_Flash_Init(void)
{
    OP_BEGIN();
    FLASH_CS_HIGH();

    /* Флеш сидит на постоянном питании, поэтому Deep Power-Down переживает сброс МК:
     * если ресет/перепрошивка случились во время сна, чип всё ещё в DPD и игнорирует
     * любые команды, кроме 0xAB. Будим его до первого обращения — иначе
     * SPI_Flash_WaitBusy() ниже будет впустую ждать до самого таймаута. */
    SPI_Flash_ReleasePowerDown();

    /* Снять защиту блоков, иначе запись/стирание будут игнорироваться.
     * Неудача здесь не фатальна: журнал снимает защиту перед каждой записью
     * (SPI_Flash_Unlock) и откажет там, если она так и не снялась. */
    (void)SPI_Flash_GlobalUnprotect();
    OP_END();
}

uint8_t SPI_Flash_Probe(void)
{
    uint8_t cmd = FLASH_CMD_RDID;
    uint8_t id[3] = {0};

    OP_BEGIN();
    FLASH_CS_LOW();
    HAL_StatusTypeDef st = HAL_SPI_Transmit(&hspi1, &cmd, 1, FLASH_SPI_TIMEOUT_MS);
    if (st == HAL_OK)
        st = HAL_SPI_Receive(&hspi1, id, sizeof(id), FLASH_SPI_TIMEOUT_MS);
    FLASH_CS_HIGH();
    OP_END();

    /* Конкретный ID не проверяем: припаять могут другую совместимую микросхему,
     * и жёсткая сверка превратилась бы в ложный отказ. Нас интересует только
     * одно — есть ли на шине хоть кто-то. Молчащая линия читается как сплошные
     * нули (подтянута вниз) либо сплошные единицы (подтянута вверх). */
    if (st != HAL_OK ||
        (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) ||
        (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF))
    {
        probed_ok = 0;
        return 0;
    }

    probed_ok = 1;
    return 1;
}

uint8_t SPI_Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4];

    cmd[0] = FLASH_CMD_READ;
    cmd[1] = (addr >> 16) & 0xFF;
    cmd[2] = (addr >> 8)  & 0xFF;
    cmd[3] = addr & 0xFF;

    OP_BEGIN();
    /* Занятая стиранием флеш на READ отвечает мусором, а RMW записал бы его
     * обратно в соседние сектора. Сначала дожидаемся конца прошлой операции. */
    uint8_t ok = SPI_Flash_WaitBusy();
    if (ok)
    {
        FLASH_CS_LOW();
        ok = (HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS) == HAL_OK) &&
             (HAL_SPI_Receive(&hspi1, buf, len, FLASH_SPI_TIMEOUT_MS) == HAL_OK);
        FLASH_CS_HIGH();
    }
    OP_END();
    return ok;
}

uint8_t SPI_Flash_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t ok = 1;

    OP_BEGIN();
    while (ok && len > 0)
    {
        uint32_t page_off = addr % FLASH_PAGE_SIZE;
        uint32_t chunk    = FLASH_PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;

        ok = SPI_Flash_WaitBusy() && SPI_Flash_WriteEnable();
        if (!ok)
            break;

        uint8_t cmd[4];
        cmd[0] = FLASH_CMD_PP;
        cmd[1] = (addr >> 16) & 0xFF;
        cmd[2] = (addr >> 8)  & 0xFF;
        cmd[3] = addr & 0xFF;

        FLASH_CS_LOW();
        ok = (HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS) == HAL_OK) &&
             (HAL_SPI_Transmit(&hspi1, (uint8_t*)buf, chunk, FLASH_SPI_TIMEOUT_MS) == HAL_OK);
        FLASH_CS_HIGH();

        /* Таймаут программирования — отказ, а не повод идти дальше: следующая
         * страница ушла бы в занятый чип и молча пропала. */
        if (ok)
            ok = SPI_Flash_WaitBusy();

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    OP_END();
    return ok;
}

uint8_t SPI_Flash_Verify(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t chunk_buf[64];
    uint8_t ok = 1;

    OP_BEGIN();
    while (ok && len > 0)
    {
        uint32_t chunk = (len > sizeof(chunk_buf)) ? sizeof(chunk_buf) : len;
        ok = SPI_Flash_Read(addr, chunk_buf, chunk) &&
             (memcmp(chunk_buf, buf, chunk) == 0);
        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    OP_END();
    return ok;
}

uint8_t SPI_Flash_EraseSector(uint32_t addr)
{
    /* выровнять по 4KB */
    addr &= ~(FLASH_SECTOR_SIZE - 1);

    OP_BEGIN();
    /* WREN, поданный занятому чипу, игнорируется — тогда и стирание не пройдёт. */
    uint8_t ok = SPI_Flash_WaitBusy() && SPI_Flash_WriteEnable();

    if (ok)
    {
        uint8_t cmd[4];
        cmd[0] = FLASH_CMD_SE_4K;
        cmd[1] = (addr >> 16) & 0xFF;
        cmd[2] = (addr >> 8)  & 0xFF;
        cmd[3] = addr & 0xFF;

        FLASH_CS_LOW();
        ok = (HAL_SPI_Transmit(&hspi1, cmd, 4, FLASH_SPI_TIMEOUT_MS) == HAL_OK);
        FLASH_CS_HIGH();
    }

    if (ok)
        ok = SPI_Flash_WaitBusy();
    OP_END();
    return ok;
}

void SPI_Flash_SetBusy(uint8_t busy)
{
    session_busy = busy ? 1U : 0U;
}

uint8_t SPI_Flash_IsBusy(void)
{
    return (session_busy || op_depth) ? 1U : 0U;
}

uint8_t SPI_Flash_IsReady(void)
{
    return (probed_ok && !in_dpd) ? 1U : 0U;
}

void SPI_Flash_NoteHostSession(void)
{
    host_session = 1;
}

uint8_t SPI_Flash_TakeHostSession(void)
{
    /* Проверка и сброс — под запретом прерываний: иначе STORAGE_Init_FS из
     * прерывания OTG между чтением и обнулением потерял бы отметку о сессии
     * (R8). PRIMASK восстанавливается, а не снимается: функцию могут позвать
     * и при уже запрещённых прерываниях. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint8_t was = host_session;
    host_session = 0;
    __set_PRIMASK(primask);
    return was;
}
