/* Тесты чистой логики FieldSensor, собираются и запускаются на обычном
 * компьютере — прошивать плату не нужно.
 *
 *     ./tools/test.sh
 *
 * Сюда попадает только то, что не трогает регистры: пересчёт напряжений,
 * разбор ответа датчика, CRC. Работа с шинами проверяется на живой плате,
 * её тестами не подменить. Смысл разделения в том, что именно арифметика и
 * разбор дают ошибки, которые на глаз не видно, — а стоят они дорого:
 * из-за одной такой в архив писались правдоподобные 0.00 °C с мёртвой шины.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "aht20_decode.h"
#include "battery_curve.h"
#include "ds18b20_decode.h"
#include "power_policy.h"
#include "sensor_policy.h"
#include "soil_curve.h"
#include "storage_policy.h"
#define LOG_RING_IMPLEMENTATION
#include "log_ring.h"
#include <stdlib.h>

#define BTN_TEST_POLL_MS 100U   /* шаг опроса кнопки в прошивке */

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
}

static void eqf(float got, float want, float tol, const char *what)
{
    checks++;
    if (!(fabsf(got - want) <= tol)) {
        failures++;
        printf("  FAIL  %s: получено %.4f, ожидалось %.4f\n", what, got, want);
    }
}

static void equ(unsigned got, unsigned want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL  %s: получено %u, ожидалось %u\n", what, got, want);
    }
}

/* ---------------------------------------------------------------- DS18B20 */

/* Эталонные scratchpad'ы в раскладке даташита: температура (2 байта), TH, TL,
 * конфигурация 0x7F (12 бит), 0xFF, байт 6 (0x10 - младший полубайт
 * температуры после конвертации, 0x0C по сбросу), 0x10, CRC. Контрольный
 * байт посчитан независимой реализацией Dallas CRC-8 на Python, а не этим же
 * кодом, иначе тест проверял бы сам себя; для SP_85C он совпадает с
 * общеизвестным кадром по сбросу 50 05 4B 46 7F FF 0C 10 1C. */
static const unsigned char SP_25C[9]  = {0x91,0x01,0x4B,0x46,0x7F,0xFF,0x0F,0x10,0x25};
static const unsigned char SP_NEG[9]  = {0x5E,0xFF,0x4B,0x46,0x7F,0xFF,0x02,0x10,0xB6};
static const unsigned char SP_85C[9]  = {0x50,0x05,0x4B,0x46,0x7F,0xFF,0x0C,0x10,0x1C};
static const unsigned char SP_0C[9]   = {0x00,0x00,0x4B,0x46,0x7F,0xFF,0x10,0x10,0x69};
static const unsigned char SP_85C1[9] = {0x51,0x05,0x4B,0x46,0x7F,0xFF,0x0F,0x10,0x0A};
static const unsigned char SP_125[9]  = {0xD0,0x07,0x4B,0x46,0x7F,0xFF,0x10,0x10,0x55};
static const unsigned char SP_125P[9] = {0xD1,0x07,0x4B,0x46,0x7F,0xFF,0x0F,0x10,0xE2};
static const unsigned char SP_M55[9]  = {0x90,0xFC,0x4B,0x46,0x7F,0xFF,0x10,0x10,0xEE};
static const unsigned char SP_M55M[9] = {0x8F,0xFC,0x4B,0x46,0x7F,0xFF,0x01,0x10,0x68};
/* Те же 25 °C с верным CRC, но с испорченными служебными байтами. */
static const unsigned char SP_CFG9[9] = {0x91,0x01,0x4B,0x46,0x1F,0xFF,0x0F,0x10,0xB5};
static const unsigned char SP_CFGX[9] = {0x91,0x01,0x4B,0x46,0xFF,0xFF,0x0F,0x10,0xFC};
static const unsigned char SP_B5X[9]  = {0x91,0x01,0x4B,0x46,0x7F,0x0C,0x0F,0x10,0x4C};
static const unsigned char SP_B7X[9]  = {0x91,0x01,0x4B,0x46,0x7F,0xFF,0x0F,0x00,0xB8};

static void test_ds18b20(void)
{
    float t = -999.0f;
    unsigned char sp[9];

    puts("DS18B20");

    equ(DS18B20_CRC8(SP_25C, 8), 0x25, "CRC8 совпал с независимым расчётом");
    equ(DS18B20_CRC8(SP_85C, 8), 0x1C, "CRC8 кадра по сбросу совпал с общеизвестным");

    equ(DS18B20_DecodeScratchpad(SP_25C, &t), DS18B20_OK, "исправный ответ принят");
    eqf(t, 25.0625f, 0.0001f, "+25.0625 C");

    /* Отрицательные температуры — это дополнительный код в двух байтах.
     * Без явного приведения к int16_t они превращались бы в +4000 с копейками,
     * а зимняя теплица — ровно тот случай, где это вылезет. */
    equ(DS18B20_DecodeScratchpad(SP_NEG, &t), DS18B20_OK, "морозный ответ принят");
    eqf(t, -10.125f, 0.0001f, "-10.125 C");

    /* Та самая ошибка, которая доехала до архива: CRC8 от девяти нулей равен
     * нулю, поэтому мёртвая шина проходила проверку и давала «валидные» 0.00 C —
     * для почвы совершенно правдоподобное значение. */
    memset(sp, 0, sizeof(sp));
    equ(DS18B20_DecodeScratchpad(sp, &t), DS18B20_ERR_ALL_ZERO,
        "сплошные нули отвергнуты, а не приняты за 0.00 C");

    /* А вот честный ноль (с настоящим CRC и ненулевым остатком scratchpad)
     * обязан пройти: иначе прибор ослепнет ровно на точке замерзания. */
    equ(DS18B20_DecodeScratchpad(SP_0C, &t), DS18B20_OK, "настоящий 0.00 C принят");
    eqf(t, 0.0f, 0.0001f, "0.00 C");

    memcpy(sp, SP_25C, sizeof(sp));
    sp[3] ^= 0x01;
    equ(DS18B20_DecodeScratchpad(sp, &t), DS18B20_ERR_CRC, "битый байт пойман по CRC");

    memcpy(sp, SP_25C, sizeof(sp));
    sp[8] ^= 0xFF;
    equ(DS18B20_DecodeScratchpad(sp, &t), DS18B20_ERR_CRC, "битый CRC пойман");

    /* Почти пустой кадр с подобранным CRC раньше проходил всегда (255 из 255
     * вариантов «только sp[0] != 0»). Служебные байты мусор не воспроизводит. */
    {
        int accepted = 0;
        for (int x = 1; x < 256; x++) {
            memset(sp, 0, sizeof(sp));
            sp[0] = (unsigned char)x;
            sp[8] = DS18B20_CRC8(sp, 8);
            if (DS18B20_DecodeScratchpad(sp, &t) == DS18B20_OK) accepted++;
        }
        equ((unsigned)accepted, 0, "почти пустые кадры с верным CRC отвергнуты");
    }
    equ(DS18B20_DecodeScratchpad(SP_CFGX, &t), DS18B20_ERR_RESERVED,
        "старший бит конфигурации поднят — отвергнуто");
    equ(DS18B20_DecodeScratchpad(SP_B5X, &t), DS18B20_ERR_RESERVED, "байт 5 не 0xFF — отвергнуто");
    equ(DS18B20_DecodeScratchpad(SP_B7X, &t), DS18B20_ERR_RESERVED, "байт 7 не 0x10 — отвергнуто");
    /* Биты разрешения в конфигурации законно меняются: 9 бит — тоже DS18B20. */
    equ(DS18B20_DecodeScratchpad(SP_CFG9, &t), DS18B20_OK, "конфигурация 9 бит принята");

    /* 85.00 C — значение, которое DS18B20 отдаёт после подачи питания, если
     * преобразование не выполнялось, и с верным CRC. Датчик обесточивается
     * каждый час, так что непринятая CONVERT T даёт ровно этот кадр. Раньше
     * тест называл его валидным; в почве теплицы 85 C не бывает. */
    t = -999.0f;
    equ(DS18B20_DecodeScratchpad(SP_85C, &t), DS18B20_ERR_RESET_VALUE,
        "85.00 C по сбросу отвергнуты");
    eqf(t, -999.0f, 0.0001f, "при отказе температура не тронута");
    /* Соседний код — уже настоящее измерение: отсеивается ровно одно значение. */
    equ(DS18B20_DecodeScratchpad(SP_85C1, &t), DS18B20_OK, "85.0625 C принято");
    eqf(t, 85.0625f, 0.0001f, "+85.0625 C");

    /* Шкала датчика -55..+125: края проходят, шаг наружу — нет. */
    equ(DS18B20_DecodeScratchpad(SP_125, &t),  DS18B20_OK,        "+125 C принято");
    equ(DS18B20_DecodeScratchpad(SP_125P, &t), DS18B20_ERR_RANGE, "+125.0625 C отвергнуто");
    equ(DS18B20_DecodeScratchpad(SP_M55, &t),  DS18B20_OK,        "-55 C принято");
    eqf(t, -55.0f, 0.0001f, "-55 C");
    equ(DS18B20_DecodeScratchpad(SP_M55M, &t), DS18B20_ERR_RANGE, "-55.0625 C отвергнуто");
}

/* --------------------------------------------------------------- батарея */

static void test_battery(void)
{
    puts("Батарея");

    equ(Battery_VoltsToPercent(4.20f), 100, "полный заряд");
    equ(Battery_VoltsToPercent(5.00f), 100, "выше таблицы — не больше 100");
    equ(Battery_VoltsToPercent(3.00f), 0,   "нижняя граница таблицы");
    equ(Battery_VoltsToPercent(0.00f), 0,   "ниже таблицы — не меньше 0");
    equ(Battery_VoltsToPercent(-1.0f), 0,   "отрицательное напряжение не ломает шкалу");

    /* Узлы таблицы обязаны воспроизводиться точно: интерполяция не должна
     * «съезжать» на краях отрезков. */
    equ(Battery_VoltsToPercent(4.10f), 95, "узел 4.10 В");
    equ(Battery_VoltsToPercent(3.70f), 52, "узел 3.70 В");
    equ(Battery_VoltsToPercent(3.30f), 6,  "узел 3.30 В");

    /* Середина отрезка 3.80(65 %) .. 3.70(52 %) — ровно половина между ними. */
    equ(Battery_VoltsToPercent(3.75f), 59, "середина отрезка 3.80-3.70");

    /* Монотонность: процент не имеет права расти при падении напряжения.
     * Немонотонная кривая означала бы «заряд вырос сам собой» в архиве. */
    int prev = 101, bad = 0;
    for (int mv = 4500; mv >= 2500; mv--) {
        int p = Battery_VoltsToPercent((float)mv / 1000.0f);
        if (p > prev) bad++;
        prev = p;
    }
    ok(bad == 0, "кривая монотонна на всём диапазоне 2.5-4.5 В");

    /* NAN раньше проскакивал все сравнения и становился 0 % — «батарея пуста». */
    equ(Battery_VoltsToPercent(NAN), BAT_PERCENT_UNKNOWN, "NAN — «неизвестно», а не 0 %");

    /* Пересчёт VREFINT -> VDD -> вольты банки. Калибровка 1500 отсчётов при
     * 3.30 В — типичное значение для F401. */
    {
        float vdd = -1.0f, v = -1.0f;

        equ(Battery_ComputeVolts(2358, 1500, 1500, &vdd, &v), 1, "обычный замер принят");
        eqf(vdd, 3.30f, 0.0005f, "VDD = 3.30 В, когда опора совпала с калибровкой");
        eqf(v, 3.8004f, 0.0005f, "2358 отсчётов при 3.30 В и делителе 2 = 3.80 В");

        /* Банка просела ниже дропаута LDO, VDD поехал вниз — опора это видит.
         * Без пересчёта через VREFINT вышло бы 4.40 В вместо 4.00. */
        equ(Battery_ComputeVolts(2730, 1650, 1500, &vdd, &v), 1, "замер при просевшем VDD принят");
        eqf(vdd, 3.00f, 0.0005f, "VDD = 3.00 В по опоре");
        eqf(v, 4.00f, 0.0005f, "напряжение банки посчитано от реального VDD");

        /* Отказы: в полях NAN, а не нули. */
        equ(Battery_ComputeVolts(2358, 0, 1500, &vdd, &v), 0, "нулевая опора — отказ, без деления на ноль");
        ok(isnan(vdd) && isnan(v), "при нулевой опоре VDD и вольты — NAN");
        vdd = v = -1.0f;
        equ(Battery_ComputeVolts(0, 1500, 1500, &vdd, &v), 0, "0 отсчётов (оборван верх делителя) — отказ");
        ok(isnan(v), "при обрыве делителя вольты — NAN");
        /* Опора при этом цела, и VDD известно — по нему хранилище решает о записи (R2). */
        eqf(vdd, 3.30f, 0.0005f, "при обрыве делителя VDD по опоре всё равно отдано");
        equ(Battery_ComputeVolts(4094, 1500, 1500, &vdd, &v), 0, "~6.6 В (оборван низ делителя) — отказ");
        /* Насыщение при низком VDD даёт правдоподобные 4.00 В — ловит только
         * проверка самого насыщения. */
        equ(Battery_ComputeVolts(4095, 2475, 1500, &vdd, &v), 0, "насыщенный канал при VDD 2.0 В — отказ, а не 4.00 В");
        equ(Battery_ComputeVolts(4094, 2475, 1500, &vdd, &v), 1, "4094 при VDD 2.0 В — ещё замер");

        /* Края окон числами: тихий сдвиг порога обязан ронять тест. */
        equ(Battery_ComputeVolts(2200, 1376, 1500, &vdd, &v), 1, "VDD 3.597 В — принято");
        equ(Battery_ComputeVolts(2200, 1374, 1500, &vdd, &v), 0, "VDD 3.603 В — за абсолютным максимумом, отказ");
        equ(Battery_ComputeVolts(3000, 2749, 1500, &vdd, &v), 1, "VDD 1.801 В — принято");
        equ(Battery_ComputeVolts(3000, 2751, 1500, &vdd, &v), 0, "VDD 1.799 В — МК так не работает, отказ");
        equ(Battery_ComputeVolts(1552, 1500, 1500, &vdd, &v), 1, "банка 2.501 В — принято");
        equ(Battery_ComputeVolts(1551, 1500, 1500, &vdd, &v), 0, "банка 2.500- В — отказ");
        equ(Battery_ComputeVolts(2729, 1500, 1500, &vdd, &v), 1, "банка 4.398 В — принято");
        equ(Battery_ComputeVolts(2731, 1500, 1500, &vdd, &v), 0, "банка 4.402 В — отказ");
    }
}

/* ----------------------------------------------------------------- почва */

static void test_soil(void)
{
    puts("Влажность почвы");

    eqf(Soil_VoltageToMoisture(SOIL_AIR_VOLTS),   0.0f,   0.001f, "сухой воздух = 0 %");
    eqf(Soil_VoltageToMoisture(SOIL_WATER_VOLTS), 100.0f, 0.001f, "вода = 100 %");
    eqf(Soil_VoltageToMoisture((SOIL_AIR_VOLTS + SOIL_WATER_VOLTS) / 2.0f),
        50.0f, 0.001f, "середина калибровки = 50 %");

    /* Датчик ёмкостный и уходит за калибровку от температуры и от разброса
     * экземпляров. Отдать наружу -7 % или 113 % значило бы записать в архив
     * заведомо ложное число вместо честной границы шкалы. Но прижимается
     * только то, что лежит внутри окна правдоподобия. */
    eqf(Soil_VoltageToMoisture(SOIL_AIR_VOLTS + 0.05f),   0.0f,   0.001f,
        "чуть суше калибровки прижато к 0");
    eqf(Soil_VoltageToMoisture(SOIL_WATER_VOLTS - 0.05f), 100.0f, 0.001f,
        "чуть мокрее калибровки прижато к 100");

    /* Обрыв питания или сигнала даёт 0 В, обрыв земли — напряжение питания.
     * Раньше оба случая уходили в архив ровными 100 % и 0 % — правдоподобными
     * «полили» и «пересохло». Исправный датчик так не показывает. */
    ok(isnan(Soil_VoltageToMoisture(0.00f)), "0 В (обрыв) — NAN, а не 100 %");
    ok(isnan(Soil_VoltageToMoisture(3.30f)), "3.3 В (обрыв земли) — NAN, а не 0 %");
    ok(isnan(Soil_VoltageToMoisture(NAN)),   "NAN от АЦП так и остаётся NAN");

    /* Края окна (0.40 и 3.00 В) числами, а не макросами: тест должен заметить,
     * если порог молча сдвинут. Внутри — прижатое число, за краем на 10 мВ —
     * уже отказ. */
    eqf(Soil_VoltageToMoisture(0.41f), 100.0f, 0.001f, "0.41 В — ещё число (прижато к 100)");
    ok(isnan(Soil_VoltageToMoisture(0.39f)), "0.39 В — за нижним краем окна, NAN");
    eqf(Soil_VoltageToMoisture(2.99f), 0.0f, 0.001f, "2.99 В — ещё число (прижато к 0)");
    ok(isnan(Soil_VoltageToMoisture(3.01f)), "3.01 В — за верхним краем окна, NAN");
}

/* ------------------------------------------------------- уход в STOP */

/* Счётчик для Policy_CounterMoves: значение 0, начиная с чтения at — 1. */
typedef struct { uint32_t at; uint32_t calls; } Fake_Counter;

static uint32_t Fake_CounterRead(void *ctx)
{
    Fake_Counter *c = (Fake_Counter *)ctx;
    uint32_t v = (c->at != 0U && c->calls >= c->at) ? 1U : 0U;
    c->calls++;
    return v;
}

static void test_sleep_policy(void)
{
    const uint32_t MAX = 300000U;   /* тот же потолок, что и в прошивке: 5 минут */
    Policy_Button b = {0};

    puts("Уход в сон: кнопка");

    equ(Policy_ButtonKeepsAwake(&b, 0, 0, MAX), 0, "кнопку не держат — спать можно");
    equ(Policy_ButtonKeepsAwake(&b, 1, 0, MAX), 1, "кнопку зажали — не спим");
    equ(Policy_ButtonKeepsAwake(&b, 1, MAX - 1, MAX), 1, "потолок не достигнут — не спим");

    /* Потолок нужен на случай залипшей или залитой водой кнопки: без него
     * прибор не уснул бы никогда и высадил бы батарею за несколько дней. */
    equ(Policy_ButtonKeepsAwake(&b, 1, 1, MAX), 0, "кнопка залипла — потолок сработал");
    equ(b.stuck, 1, "залипание запомнено");

    /* Ради этого правило и переписано (A1). Раньше потолок отсчитывался
     * заново в каждом цикле удержания, и залипшая кнопка держала плату по
     * 5 минут КАЖДЫЙ час. Новый цикл начинается с dt = 0 — проверяем, что
     * ни в одном следующем цикле она больше не держит. */
    for (int cycle = 0; cycle < 24; cycle++)
    {
        uint8_t awake = Policy_ButtonKeepsAwake(&b, 1, 0, MAX);
        awake |= Policy_ButtonKeepsAwake(&b, 1, BTN_TEST_POLL_MS, MAX);
        if (awake) { ok(0, "залипшая кнопка снова держит в следующем цикле"); break; }
    }
    equ(b.stuck, 1, "сутки циклов — кнопка всё ещё признана залипшей");

    /* Отпустили — следующее нажатие снова честное, с полным потолком. */
    equ(Policy_ButtonKeepsAwake(&b, 0, 0, MAX), 0, "отпустили — спим");
    equ(b.stuck, 0, "отпускание снимает залипание");
    equ(b.held_ms, 0, "отпускание обнуляет счёт удержания");
    equ(Policy_ButtonKeepsAwake(&b, 1, 0, MAX), 1, "после отпускания нажатие снова держит");

    /* Удержание копится через циклы: две половины потолка в разных циклах —
     * это один потолок, а не два. */
    Policy_Button c = {0};
    Policy_ButtonKeepsAwake(&c, 1, 0, MAX);
    equ(Policy_ButtonKeepsAwake(&c, 1, MAX / 2, MAX), 1, "половина потолка — ещё держит");
    Policy_ButtonKeepsAwake(&c, 1, 0, MAX);   /* следующий цикл, dt = 0 */
    equ(Policy_ButtonKeepsAwake(&c, 1, MAX / 2, MAX), 0, "вторая половина в другом цикле — потолок");

    /* Огромный dt не переполняет счёт и не «обнуляет» его. */
    Policy_Button d = {0};
    Policy_ButtonKeepsAwake(&d, 1, MAX - 10, MAX);
    equ(Policy_ButtonKeepsAwake(&d, 1, 0xFFFFFFF0U, MAX), 0, "dt у края uint32 — потолок, не переполнение");

    /* Потолок задаётся снаружи; нулевой означает «не ждать вовсе». */
    Policy_Button z = {0};
    equ(Policy_ButtonKeepsAwake(&z, 1, 0, 0), 0, "нулевой потолок не заставляет ждать");
}

static void test_usb_policy(void)
{
    const uint32_t ENUM = 10000U;   /* USB_ENUM_MAX_MS в прошивке */
    Policy_Usb u = {0};

    puts("Уход в сон: USB");

    equ(Policy_UsbKeepsAwake(&u, 0, 0, 0, 0, ENUM), 0, "VBUS нет — USB не держит");

    /* Хост перечислил — держим сколько угодно: плата питается от него, а
     * уход в STOP оборвал бы чтение. */
    equ(Policy_UsbKeepsAwake(&u, 1, 1, 1, 0, ENUM), 1, "хост перечислил — не спим");
    equ(Policy_UsbKeepsAwake(&u, 1, 1, 0, ENUM * 100, ENUM), 1, "сессия длинная — всё равно не спим");

    /* VBUS без хоста (A9): зарядка, пауэрбанк, влага на делителе. Ждём
     * перечисления не дольше потолка и уходим спать. */
    Policy_Usb c = {0};
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 1, 0, ENUM), 1, "VBUS появился — ждём хоста");
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 0, ENUM - 1, ENUM), 1, "хоста ещё нет, потолок не достигнут");
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 0, 1, ENUM), 0, "хоста нет 10 с — это зарядка, спим");

    /* Следующие пробуждения с тем же VBUS — сразу спать, без новых 10 с:
     * на ложном VBUS каждое такое ожидание стоило бы батарее. */
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 0, 0, ENUM), 0, "зарядка и в следующем цикле — спим сразу");

    /* Вставили кабель заново (фронт VBUS) — у нового хоста свой шанс. */
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 1, 0, ENUM), 1, "фронт VBUS — снова ждём хоста");
    Policy_UsbKeepsAwake(&c, 1, 0, 0, ENUM, ENUM);
    equ(Policy_UsbKeepsAwake(&c, 0, 0, 0, 0, ENUM), 0, "кабель вынули — спим");
    equ(Policy_UsbKeepsAwake(&c, 1, 0, 0, 0, ENUM), 1, "VBUS пропадал — прошлая неудача не в счёт");

    /* Хост ушёл в сон (SUSPENDED, не CONFIGURED) — отсчёт идёт заново. */
    Policy_Usb h = {0};
    Policy_UsbKeepsAwake(&h, 1, 0, 1, 0, ENUM);
    Policy_UsbKeepsAwake(&h, 1, 0, 0, ENUM - 2, ENUM);   /* перечислился в последний миг */
    Policy_UsbKeepsAwake(&h, 1, 1, 0, 1, ENUM);
    equ(Policy_UsbKeepsAwake(&h, 1, 0, 0, ENUM - 1, ENUM), 1, "хост уснул — ещё ждём");
    equ(Policy_UsbKeepsAwake(&h, 1, 0, 0, 1, ENUM), 0, "хост спит дольше потолка — спим и мы");

    /* Замер по расписанию, пока плата держится на шине (C8). */
    equ(Policy_MeasureDue(3599999U, 3600000U), 0, "интервал не прошёл — замера нет");
    equ(Policy_MeasureDue(3600000U, 3600000U), 1, "прошёл интервал — замер, не засыпая");
}

static void test_watchdog_policy(void)
{
    puts("Сторож и причина сброса");

    /* Час сна кусками по 15 с: ровно 240 кусков, в сумме ровно час —
     * пинки сторожу не сдвигают расписание замеров. */
    uint32_t left = 3600U, n = 0U, longest = 0U;
    while (left > 0U && n < 10000U)
    {
        uint32_t c = Policy_SleepChunkSec(left, 15U);
        if (c == 0U) break;
        if (c > longest) longest = c;
        left -= c; n++;
    }
    equ(n, 240U, "3600 с по 15 с — 240 кусков");
    equ(left, 0U, "куски складываются ровно в интервал");
    equ(longest, 15U, "ни один кусок не длиннее шага сторожа");

    /* Интервал, не кратный шагу: последний кусок — остаток. */
    equ(Policy_SleepChunkSec(37U, 15U), 15U, "37 с: первый кусок 15");
    equ(Policy_SleepChunkSec(7U, 15U), 7U, "остаток 7 с — кусок 7, а не 15");
    equ(Policy_SleepChunkSec(15U, 15U), 15U, "ровно шаг — один кусок");
    equ(Policy_SleepChunkSec(0U, 15U), 0U, "спать нечего — 0");

    /* S1: RTC от остановившегося LSE. Счётчик меняется на чтении номер at
     * (с нуля), at = 0 — не меняется никогда. */
    puts("Идёт ли RTC (остановка LSE)");
    {
        Fake_Counter c;
        c = (Fake_Counter){0, 0};
        equ(Policy_CounterMoves(Fake_CounterRead, &c, 100U), 0, "кварц встал — SSR стоит, откат на LSI");
        equ(c.calls, 100U, "стоящий счётчик опрошен все 100 раз, не меньше");
        c = (Fake_Counter){1, 0};
        equ(Policy_CounterMoves(Fake_CounterRead, &c, 100U), 1, "SSR сменился сразу — RTC идёт");
        c = (Fake_Counter){99, 0};
        equ(Policy_CounterMoves(Fake_CounterRead, &c, 100U), 1, "SSR сменился на последнем чтении — идёт");
        c = (Fake_Counter){100, 0};
        equ(Policy_CounterMoves(Fake_CounterRead, &c, 100U), 0, "сменился бы только за окном — не идёт");
    }

    /* Флаги RCC_CSR после подачи питания на F401: POR + PIN + BOR. */
    const uint32_t POR = POLICY_CSR_PORRSTF | POLICY_CSR_PINRSTF | POLICY_CSR_BORRSTF;
    equ(Policy_ResetCause(POR, 0), 7U, "подача питания — POR|PIN|BOR");
    equ(Policy_ResetCause(POLICY_CSR_PINRSTF, 0), 2U, "кнопка RESET — PIN");
    equ(Policy_ResetCause(POLICY_CSR_IWDGRSTF | POLICY_CSR_PINRSTF, 0), 18U, "сторож — IWDG (и PIN)");
    equ(Policy_ResetCause(POLICY_CSR_LPWRRSTF | POLICY_CSR_WWDGRSTF, 0), 96U, "WWDG и LPWR — свои биты");

    /* HardFault: обработчик оставил код и сбросил программно. */
    equ(Policy_ResetCause(POLICY_CSR_SFTRSTF | POLICY_CSR_PINRSTF,
                          POLICY_FAULT_MAGIC | POLICY_FAULT_HARD),
        8U | 2U | (POLICY_FAULT_HARD << 8), "HardFault — SFT и код 1 в старшем байте");

    /* Error_Handler сбрасывает сторожем — его код тоже засчитывается. */
    equ(Policy_ResetCause(POLICY_CSR_IWDGRSTF, POLICY_FAULT_MAGIC | POLICY_FAULT_ERROR),
        16U | (POLICY_FAULT_ERROR << 8), "Error_Handler — IWDG и код 6");

    /* Код от прошлого раза при сбросе питанием — не причина этого старта. */
    equ(Policy_ResetCause(POR, POLICY_FAULT_MAGIC | POLICY_FAULT_HARD), 7U,
        "старый код сбоя при подаче питания не засчитывается");

    /* Без метки — мусор или ноль backup-домена. */
    equ(Policy_ResetCause(POLICY_CSR_SFTRSTF, 0x00000001U), 8U, "код без метки — не код");
    equ(Policy_ResetCause(POLICY_CSR_SFTRSTF, POLICY_FAULT_MAGIC | 0x1234U), 8U,
        "код вне диапазона — не код");
}

/* Прогон одного цикла карантина так, как это делает main.c. */
static uint8_t quarantine_cycle(Policy_Quarantine *q, uint8_t rail_dead, uint32_t skip)
{
    uint8_t powered = Policy_SensorsPowerAllowed(q);
    Policy_QuarantineUpdate(q, powered, powered && rail_dead, skip);
    return powered;
}

static void test_quarantine_policy(void)
{
    const uint32_t SKIP = 6U;   /* 6 часов при часовом интервале */
    Policy_Quarantine q = {0};

    puts("Карантин питания датчиков");

    equ(quarantine_cycle(&q, 1, SKIP), 1, "1-й отказ — питание подаём");
    equ(quarantine_cycle(&q, 1, SKIP), 1, "2-й отказ — подаём");
    equ(quarantine_cycle(&q, 1, SKIP), 1, "3-й отказ — подаём, но это последний");

    /* Дальше SKIP циклов без питания — и ровно SKIP, не больше и не меньше. */
    unsigned off = 0;
    while (!Policy_SensorsPowerAllowed(&q) && off < 100U) { quarantine_cycle(&q, 1, SKIP); off++; }
    equ(off, SKIP, "карантин длится ровно SKIP циклов");

    /* Пробная попытка не удалась — сразу обратно в карантин, без двух
     * лишних циклов на полном токе. */
    equ(quarantine_cycle(&q, 1, SKIP), 1, "после карантина — одна попытка");
    equ(Policy_SensorsPowerAllowed(&q), 0, "попытка не удалась — снова карантин");

    /* Отработали карантин, попытка удалась — всё по-старому: следующий
     * одиночный отказ карантина не вызывает. */
    for (unsigned i = 0; i < SKIP; i++) quarantine_cycle(&q, 1, SKIP);
    equ(quarantine_cycle(&q, 0, SKIP), 1, "рельса ожила — питание есть");
    equ(quarantine_cycle(&q, 1, SKIP), 1, "одиночный отказ после выздоровления — питание есть");
    equ(Policy_SensorsPowerAllowed(&q), 1, "одиночный отказ карантина не включает");

    /* Отказы не подряд — не карантин. */
    Policy_Quarantine r = {0};
    for (unsigned i = 0; i < 20U; i++)
        quarantine_cycle(&r, (i % 3U) != 2U, SKIP);   /* два отказа, успех, два отказа… */
    equ(Policy_SensorsPowerAllowed(&r), 1, "два отказа и успех вперемешку — карантина нет");
}

/* ----------------------------------------------------------------- AHT20 */

/* Кадры AHT20: статус, пять байт данных, CRC-8 (полином 0x31, начало 0xFF).
 * Контрольные байты посчитаны независимой реализацией на Python (делением
 * многочленов, сверено с эталонным вектором Sensirion 0xBEEF -> 0x92),
 * а не этим же кодом, иначе тест проверял бы сам себя. */
static unsigned hexdigit(char c)
{
    return (c >= 'A') ? (unsigned)(c - 'A' + 10) : (unsigned)(c - '0');
}

static void aht_frame(unsigned char *f, const char *hex)
{
    for (int i = 0; i < AHT20_FRAME_LEN; i++)
        f[i] = (unsigned char)((hexdigit(hex[2 * i]) << 4) | hexdigit(hex[2 * i + 1]));
}

static void test_aht20(void)
{
    float t = -999.0f, h = -999.0f;
    unsigned char f[AHT20_FRAME_LEN];

    puts("AHT20");

    equ(AHT20_CRC8((const uint8_t *)"\xBE\xEF", 2), 0x92, "CRC8 совпал с эталоном Sensirion");

    /* Реальный кадр: статус без BUSY, влажность ~50 %, температура ~25 °C.
     * raw_hum = 0x80000 -> ровно 50 %, raw_temp = 0x60000 -> ровно 25 °C. */
    aht_frame(f, "1C80000600004E");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_OK, "исправный кадр принят");
    eqf(h, 50.0f, 0.01f, "влажность 50 %");
    eqf(t, 25.0f, 0.01f, "температура 25 C");

    /* Все байты полей ненулевые, ожидание точное: иначе вклад buf[2] и
     * старшего полубайта buf[3] в влажность никто не проверяет, и сдвиг
     * не на ту величину прошёл бы зелёным. raw_hum = 0x7AB3F = 502591,
     * raw_temp = 0x61234 = 397876. Допуск меньше вклада buf[3]>>4 (0.0014 %). */
    aht_frame(f, "187AB3F6123415");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_OK, "кадр со всеми битами полей принят");
    eqf(h, 47.930813f, 0.0002f, "влажность собрана из всех трёх байт");
    eqf(t, 25.888824f, 0.0002f, "температура собрана из всех трёх байт");

    /* Та ошибка, ради которой модуль и появился. Некалиброванный AHT20 отвечает
     * на шине, бит BUSY снят, транзакция успешна — но кадр пустой. Нули дают
     * ровно -50.00 C, нижний край шкалы, и такая строка два дня уходила
     * в архив как настоящее измерение воздуха. */
    aht_frame(f, "18000000000086");
    t = h = -999.0f;
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_ZERO,
        "пустой кадр отвергнут, а не разобран в -50 C");
    eqf(t, -999.0f, 0.001f, "при отказе температура не тронута");
    eqf(h, -999.0f, 0.001f, "при отказе влажность не тронута");

    /* BUSY означает «измерение ещё идёт» — это ожидание, а не поломка,
     * и отличать одно от другого важно: в первом случае надо подождать,
     * во втором ждать бесполезно. */
    aht_frame(f, "988000060000CF");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_BUSY, "BUSY отличается от пустого кадра");

    /* Ровный ноль сырой влажности при живой температуре — не сухой воздух,
     * а обнулённые байты 1..3: в теплице AHT20 такого не выдаёт. Это зеркало
     * инцидента 24 августа, и раньше тест прямо называл такой кадр нормой. */
    aht_frame(f, "18000006000090");
    t = h = -999.0f;
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_RANGE,
        "нулевая влажность при живой температуре отвергнута");
    eqf(h, -999.0f, 0.001f, "при нулевой влажности значение не тронуто");

    /* А вот нулевая ТЕМПЕРАТУРА при живой влажности законной быть не может:
     * raw_temp == 0 раскладывается ровно в -50.00 C, а мерить AHT20 умеет
     * только от -40. Реальный кадр из чистого прогона 24 августа 2026: сырая
     * влажность 8283 (0.79 %) при нулевой температуре. Проверка "оба поля в
     * нуле" его пропускала, и -50.00 C ушли в архив как настоящее измерение.
     * CRC здесь верный — отсеять кадр обязана именно шкала. */
    aht_frame(f, "180205B000003F");
    t = h = -999.0f;
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_RANGE,
        "полупустой кадр из поля отвергнут, а не разобран в -50 C");
    eqf(t, -999.0f, 0.001f, "при выходе за шкалу температура не тронута");
    eqf(h, -999.0f, 0.001f, "при выходе за шкалу влажность не тронута");

    /* Верх сырой шкалы: все единицы в 20 битах дают 150 C. Датчик столько
     * не измеряет — значит кадр битый, а не горячий. */
    aht_frame(f, "18FFFFFFFFFF17");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_RANGE, "150 C отвергнуты как выход за шкалу");

    /* Границы рабочего диапазона: крайний сырой код внутри шкалы проходит,
     * соседний снаружи — нет. Иначе отсев либо съест законные показания на
     * морозе и в жару, либо граница уедет, и тест этого не заметит.
     * raw 52429 -> -39.9998, 52428 -> -40.0001; 707788 -> 84.9998, 707789 -> 85.0002. */
    aht_frame(f, "18800000CCCD93");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_OK, "нижняя граница -40 C принята");
    eqf(t, -40.0f, 0.01f, "температура -40 C");
    aht_frame(f, "18800000CCCCA2");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_RANGE, "на 1 LSB холоднее -40 C — отвергнуто");

    aht_frame(f, "1880000ACCCC7C");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_OK, "верхняя граница +85 C принята");
    eqf(t, 85.0f, 0.01f, "температура +85 C");
    aht_frame(f, "1880000ACCCD4D");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_RANGE, "на 1 LSB горячее +85 C — отвергнуто");

    /* Кадр целый, CRC верный, но датчик сам сообщает, что не откалиброван. */
    aht_frame(f, "108000060000F9");
    equ(AHT20_DecodeFrame(f, &t, &h), AHT20_DEC_ERR_NOCAL, "кадр без бита CAL отвергнут");

    /* Любой одиночный перевёрнутый бит в любом из семи байт — не измерение.
     * Без CRC порча младших бит температуры или любой из бит влажности
     * давала правдоподобные, но ложные числа. */
    {
        unsigned char ref[AHT20_FRAME_LEN];
        int accepted = 0, crc_caught = 0;
        aht_frame(ref, "187AB3F6123415");
        for (int i = 0; i < AHT20_FRAME_LEN; i++)
            for (int bit = 0; bit < 8; bit++) {
                AHT20_DecodeStatus st;
                memcpy(f, ref, sizeof(f));
                f[i] ^= (unsigned char)(1u << bit);
                st = AHT20_DecodeFrame(f, &t, &h);
                if (st == AHT20_DEC_OK) accepted++;
                if (st == AHT20_DEC_ERR_CRC) crc_caught++;
            }
        equ((unsigned)accepted, 0, "однобитная порча кадра ни разу не принята");
        /* 55 из 56: единственный бит мимо CRC — BUSY, он проверяется первым. */
        equ((unsigned)crc_caught, 55, "однобитную порчу ловит именно CRC");
    }
}

/* ---------------------------------------------------------------- хранилище */

static void test_storage_policy(void)
{
    puts("Хранилище");

    /* MSC в прерывании: только готовая и свободная флеш. */
    equ(Storage_MscMayAccess(1, 0), 1, "готова и свободна — MSC читает");
    equ(Storage_MscMayAccess(1, 1), 0, "занята основным потоком — хосту «не готов»");
    equ(Storage_MscMayAccess(0, 0), 0, "не инициализирована — из прерывания не будим");

    /* Питание. Порог VDD 2.9 В и признак bat.low. */
    equ(Storage_MayWrite(1, 0, 3.20f), 1, "нормальное питание — пишем");
    equ(Storage_MayWrite(1, 1, 3.20f), 0, "банка ниже порога LDO — не пишем");
    equ(Storage_MayWrite(1, 0, 2.85f), 0, "VDD ниже 2.9 В — не пишем");
    equ(Storage_MayWrite(1, 0, 2.90f), 1, "ровно 2.9 В — ещё пишем");
    /* Замер не удался: ничего не знаем — архив из-за отказа АЦП не останавливаем. */
    equ(Storage_MayWrite(0, 0, NAN), 1, "замер батареи не удался — пишем");

    /* R2: глубокий разряд. 3.30f — BAT_LOW_VOLTS из battery.h (там HAL).
     * Банка ниже окна 2.5 В — замер «недостоверен», но
     * VDD по опоре известно и ниже паспорта флеш. Связка пересчёт -> решение,
     * как в main.c. Калибровка 1500 при 3.30 В. */
    {
        float vdd, v;
        uint8_t valid, low;

        /* Банка 2.45 В, LDO в дропауте, VDD 2.40 В. */
        valid = Battery_ComputeVolts(2091, 2063, 1500, &vdd, &v);
        low   = (valid && v < 3.30f) ? 1U : 0U;
        equ(valid, 0, "банка 2.45 В — вне шкалы, замер недостоверен");
        eqf(vdd, 2.40f, 0.005f, "банка 2.45 В — VDD 2.40 В всё равно известно");
        equ(Storage_MayWrite(valid, low, vdd), 0, "банка 2.45 В / VDD 2.40 В — не пишем");

        /* Банка 2.55 В, VDD 2.50 В — внутри шкалы, low=1. */
        valid = Battery_ComputeVolts(2088, 1980, 1500, &vdd, &v);
        low   = (valid && v < 3.30f) ? 1U : 0U;
        equ(valid, 1, "банка 2.55 В — замер принят");
        equ(Storage_MayWrite(valid, low, vdd), 0, "банка 2.55 В / VDD 2.50 В — не пишем");

        /* Опора не поднялась: VDD неизвестно — пишем, как и при отказе АЦП. */
        valid = Battery_ComputeVolts(2091, 0, 1500, &vdd, &v);
        equ(Storage_MayWrite(valid, 0, vdd), 1, "опора 0 — о питании ничего не знаем, пишем");

        /* Обрыв делителя при нормальном VDD — тоже пишем. */
        valid = Battery_ComputeVolts(0, 1500, 1500, &vdd, &v);
        equ(Storage_MayWrite(valid, 0, vdd), 1, "обрыв делителя при VDD 3.30 В — пишем");
    }
}

/* Загрузочные сектора и MBR, которые кладёт настоящий f_mkfs этого FatFs:
 * собран на хосте с RAM-диском на 8192 сектора, параметры как у прошлых
 * прошивок: FM_FAT, кластер 512 Б (FAT16) и кластер 1 КБ (FAT12).
 * Первые 64 байта сектора; остальное до сигнатуры — нули. Записи разделов
 * взяты из того же прогона. По ним строится старый том для теста импорта. */
static const unsigned char MKFS_BOOT16[64] = {
    0xEB,0xFE,0x90,0x4D,0x53,0x44,0x4F,0x53,0x35,0x2E,0x30,0x00,0x02,0x01,0x01,0x00,
    0x01,0x00,0x02,0xC1,0x1F,0xF8,0x20,0x00,0x3F,0x00,0xFF,0x00,0x3F,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x80,0x00,0x29,0x00,0x00,0x00,0x00,0x4E,0x4F,0x20,0x4E,0x41,
    0x4D,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x20,0x20,0x20,0x20,0x20,0x00,0x00};
static const unsigned char MKFS_PTE16[16] = {
    0x00,0x01,0x01,0x00,0x04,0xFE,0x3F,0x00,0x3F,0x00,0x00,0x00,0xC1,0x1F,0x00,0x00};
static const unsigned char MKFS_BOOT12[64] = {
    0xEB,0xFE,0x90,0x4D,0x53,0x44,0x4F,0x53,0x35,0x2E,0x30,0x00,0x02,0x02,0x01,0x00,
    0x01,0x00,0x02,0xC1,0x1F,0xF8,0x10,0x00,0x3F,0x00,0xFF,0x00,0x3F,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x80,0x00,0x29,0x00,0x00,0x00,0x00,0x4E,0x4F,0x20,0x4E,0x41,
    0x4D,0x45,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x20,0x20,0x20,0x20,0x20,0x00,0x00};
static const unsigned char MKFS_PTE12[16] = {
    0x00,0x01,0x01,0x00,0x01,0xFE,0x3F,0x00,0x3F,0x00,0x00,0x00,0xC1,0x1F,0x00,0x00};

static void test_time_trust(void)
{
    const char *B = "2026-10-22 10:00:00";   /* время сборки */
    char ts[20];

    puts("Достоверность часов");

    /* Обычный сброс — как было. */
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_NONE, 1, 0, "", B), 1, "сброс, часы шли и им верили — верим");
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_NONE, 0, 1, "", B), 0, "сброс после недоверия — не верим и дальше");

    /* Прошивка с компьютера при живом календаре. */
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_NEW_BUILD, 0, 1, "2026-10-21 09:00:00", B), 1,
        "новая сборка, календарь жив, архив раньше сборки — верим");
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_NEW_BUILD, 0, 0, "", B), 1,
        "новая сборка, архив не прочитан — верим (календарь жив)");
    /* Инкрементальная сборка со старым time_service.o: «новая» по отпечатку,
     * а время сборки — месяцы назад. Архив уже ушёл дальше — не верим. */
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_NEW_BUILD, 1, 1, "2027-01-01 00:00:00", B), 0,
        "новая сборка старше архива (старый .o) — не верим");

    /* Замена банки в поле (A6): календарь потерян, часы на времени сборки,
     * а в архиве уже месяцы строк после неё. */
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_LOST, 1, 1, "2027-03-05 14:00:01", B), 0,
        "календарь потерян, в архиве строки позже сборки — не верим");
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_LOST, 1, 1, B, B), 0,
        "последняя строка ровно во время сборки — тоже не верим");

    /* Свежая прошивка на обесточенной плате: строки только от старых сборок. */
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_LOST, 0, 1, "2026-09-01 08:00:00", B), 1,
        "архив кончается раньше сборки — свежая прошивка, верим");
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_LOST, 0, 1, "", B), 1, "архив пуст — верим времени сборки");
    equ(Policy_TimeTrusted(POLICY_TIME_SYNC_LOST, 1, 0, "", B), 0, "архив не прочитан — не верим");

    /* Метка последней строки из хвоста файла. */
    const char *t1 = "4.20,1,1,0,0,0,0\n2026-10-22 11:00:00,21.5,40.0\n2026-10-22 12:00:00,21.7,41.0\n";
    equ(Storage_LastTimestamp(t1, (uint32_t)strlen(t1), 0, ts), 1, "метка последней строки нашлась");
    ok(strcmp(ts, "2026-10-22 12:00:00") == 0, "это именно последняя строка");

    /* Хвост оборван посреди последней строки — берём её начало, если метка целиком. */
    const char *t2 = "x\n2026-10-22 11:00:00,21.5\n2026-10-22 12:0";
    equ(Storage_LastTimestamp(t2, (uint32_t)strlen(t2), 0, ts), 1, "оборванная последняя — берём предыдущую");
    ok(strcmp(ts, "2026-10-22 11:00:00") == 0, "метка предыдущей строки");

    /* Файл из одного заголовка — строк нет. */
    const char *t3 = "timestamp,air_temp,air_hum\n";
    equ(Storage_LastTimestamp(t3, (uint32_t)strlen(t3), 1, ts), 0, "только заголовок — меток нет");
    ok(ts[0] == '\0', "метка пустая");

    /* Хвост с начала файла, где первая строка — уже данные (заголовка нет). */
    const char *t4 = "2026-10-22 09:00:00,1,2";
    equ(Storage_LastTimestamp(t4, (uint32_t)strlen(t4), 1, ts), 1, "с начала файла — первая строка в счёт");
    equ(Storage_LastTimestamp(t4, (uint32_t)strlen(t4), 0, ts), 0, "не с начала — обрывок не в счёт");

    /* Похоже, но не метка. */
    const char *t5 = "\n2026-1O-22 12:00:00,1\n";   /* буква O вместо нуля */
    equ(Storage_LastTimestamp(t5, (uint32_t)strlen(t5), 0, ts), 0, "буква вместо цифры — не метка");
}

static void test_bmp280(void)
{
    puts("BMP280 и сверка воздуха");

    /* Пример из даташита BMP280 (3.12): dig_T1 = 27504, dig_T2 = 26435,
     * dig_T3 = -1000, adc_T = 519888 -> 25.08 °C. Независимый эталон. */
    eqf(Bmp280_CompensateT(519888, 27504, 26435, -1000), 25.08f, 0.001f, "пример даташита — 25.08 °C");
    ok(Bmp280_CalibOk(27504, 26435, -1000), "калибровка из даташита пригодна");

    /* Отказы, которые раньше превращались в правдоподобное число. */
    ok(isnan(Bmp280_CompensateT(0x80000, 27504, 26435, -1000)), "отсчёт 0x80000 (замера не было) — NAN");
    ok(isnan(Bmp280_CompensateT(0, 27504, 26435, -1000)), "нулевой отсчёт — NAN");
    ok(isnan(Bmp280_CompensateT(0xFFFFF, 27504, 26435, -1000)), "отсчёт из единиц (шина в воздухе) — NAN");
    ok(isnan(Bmp280_CompensateT(1000000, 27504, 26435, -1000)), "пересчёт вне −40..85 °C — NAN");
    ok(!Bmp280_CalibOk(0, 0, 0), "нулевая калибровка — непригодна");
    ok(!Bmp280_CalibOk(0xFFFF, -1, -1), "стёртая калибровка (0xFF) — непригодна");
    ok(!Bmp280_CalibOk(27504, 0, -1000), "dig_T2 = 0 — непригодна");
    ok(Bmp280_IdOk(0x58) && Bmp280_IdOk(0x60) && Bmp280_IdOk(0x56), "BMP280, BME280 и образцы — свои");
    ok(!Bmp280_IdOk(0x00) && !Bmp280_IdOk(0xFF) && !Bmp280_IdOk(0x55), "0x00/0xFF/чужой ID — не датчик");

    /* Правило сверки (порог 2.0 °C — обоснование в sensor_policy.h). */
    equ(Sensor_AirCrossCheck(21.0f, 21.9f), SENSOR_XCHK_OK, "расхождение 0.9 °C — сошлись");
    equ(Sensor_AirCrossCheck(21.0f, 23.0f), SENSOR_XCHK_OK, "ровно 2.0 °C — ещё сошлись");
    equ(Sensor_AirCrossCheck(21.0f, 23.1f), SENSOR_XCHK_DIFF, "2.1 °C — расхождение");
    equ(Sensor_AirCrossCheck(25.0f, 21.0f), SENSOR_XCHK_DIFF, "расхождение в другую сторону — тоже");
    equ(Sensor_AirCrossCheck(21.0f, NAN), SENSOR_XCHK_NO_BMP, "BMP280 не ответил — сверки нет");
    equ(Sensor_AirCrossCheck(NAN, 21.0f), SENSOR_XCHK_NO_AHT, "AHT20 nan — сверять нечего");
    equ(Sensor_AirCrossCheck(NAN, NAN), SENSOR_XCHK_NO_AHT, "оба nan — сверять нечего");
}

/* ------------------------------------------------- журнал: запись и CSV */

/* Эталон — вывод прежнего snprintf из main.c (коммит 3cfeb87, до П5а):
 * вызов скопирован из main.c скриптом дословно, собран хостовым gcc с теми
 * же типами переменных (AHT20_Data, Battery_Data, HAL_StatusTypeDef) и
 * прогнан на этих наборах. Форматтер обязан повторить его байт в байт. */
static const struct {
    LogMeas m;
    const char *line;
} CSV_GOLDEN[] = {
    { { "2026-10-22 13:00:00", 21.537f, 48.25f, 17.0625f, 63.4f, 1, 0, 87, 4.012f, 3.297f,
        1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x0104, 1, 1873.0f, 2481.0f },
      "2026-10-22 13:00:00,21.54,48.25,17.06,63.40,87,4.01,3.297,1,1,1,0,0,0,0,0,0,0,0,260,1,1873,2481\n" },
    { { "2000-01-01 00:00:00", NAN, NAN, NAN, NAN, 0, 0, 255, NAN, NAN,
        0, 0, 1, 1, 8, 3, 3, 6, 12, 3, 0, 0, NAN, NAN },
      "2000-01-01 00:00:00,nan,nan,nan,nan,nan,nan,nan,0,0,0,1,1,8,3,3,6,12,3,0,0,nan,nan\n" },
    { { "2027-01-15 04:00:01", -12.345f, 99.999f, -0.0625f, 0.0f, 1, 1, 0, 3.051f, 2.8999f,
        0, 1, 3, 2, 3, 1, 2, 21600, 4294967295UL, 4294967295UL, 0xFFFF, 4294967295UL, 0.0f, 4095.0f },
      "2027-01-15 04:00:01,-12.35,100.00,-0.06,0.00,0,3.05,2.900,0,0,1,3,2,3,1,2,21600,4294967295,4294967295,65535,4294967295,0,4095\n" },
    { { "2165-25-45 45:85:85", -0.0f, -0.004f, 85.0f, 100.0f, 0, 1, 55, 3.7f, 3.6f,
        1, 0, 0, 1, 15, 2, 1, 65535, 1, 100000, 0x8000, 123456, 4095.0f, 0.0f },
      "2165-25-45 45:85:85,-0.00,-0.00,85.00,100.00,nan,nan,3.600,0,1,0,0,1,15,2,1,65535,1,100000,32768,123456,4095,0\n" },
    { { "2099-12-31 23:59:59", 40.005f, 0.005f, -55.0f, 12.345f, 1, 0, 100, 4.2f, 3.3f,
        1, 1, 0, 0, 0, 0, 0, 0, 7, 0, 0x0024, 42, 1.0f, NAN },
      "2099-12-31 23:59:59,40.01,0.00,-55.00,12.35,100,4.20,3.300,1,1,1,0,0,0,0,0,0,7,0,36,42,1,nan\n" },
};

static void test_log_record(void)
{
    char line[LOG_CSV_LINE_MAX];
    LogRecord r;

    puts("Журнал: запись замера и строка CSV");

    equ(sizeof(LogRecord), 64U, "запись — 64 байта");
    equ(256U % LOG_RECORD_SIZE, 0U, "запись делит страницу 256 Б нацело");

    /* Формат вывода не изменился: через упаковку в 64 байта и обратно. */
    for (unsigned i = 0; i < sizeof(CSV_GOLDEN) / sizeof(CSV_GOLDEN[0]); i++)
    {
        char what[64];
        LogRecord_PackMeas(&r, &CSV_GOLDEN[i].m);
        LogRecord_Seal(&r, 1000U + i);
        equ(LogRecord_Check(&r), LOG_REC_OK, "запечатанная запись годна");
        int n = LogRecord_FormatCsv(&r, line, sizeof(line));
        snprintf(what, sizeof(what), "строка %u совпала с эталоном", i);
        ok(n > 0 && strcmp(line, CSV_GOLDEN[i].line) == 0, what);
        if (strcmp(line, CSV_GOLDEN[i].line) != 0)
            printf("    получено: %s    эталон:   %s", line, CSV_GOLDEN[i].line);
    }

    /* Заголовок — тот же набор колонок, что и был в main.c. */
    ok(strcmp(LOG_CSV_HEADER,
              "timestamp,air_temp,air_hum,soil_temp,soil_hum,bat_pct,vbat,vdd,soil_valid,lse_ok,"
              "time_ok,aht_init_st,aht_read_st,ds_err,air_xchk,i2c_rec,sens_off,write_fails,"
              "lowpower_skips,reset_cause,boot_count,soil_raw,bat_raw") == 0, "заголовок CSV прежний");
    {
        unsigned commas = 0;
        for (const char *p = LOG_CSV_HEADER; *p; p++) commas += (*p == ',');
        equ(commas + 1U, 23U, "23 колонки");
    }

    /* Целостность: любой испорченный бит, номер, тип, версия — не годна. */
    LogRecord_PackMeas(&r, &CSV_GOLDEN[0].m);
    LogRecord_Seal(&r, 7);
    equ(LogRecord_Seq(&r), 7U, "номер на месте");
    unsigned caught = 0;
    for (unsigned bit = 0; bit < LOG_RECORD_SIZE * 8U; bit++)
    {
        LogRecord x = r;
        x.b[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        caught += (LogRecord_Check(&x) == LOG_REC_BAD);
    }
    equ(caught, LOG_RECORD_SIZE * 8U, "любая одиночная ошибка бита ловится");
    {
        LogRecord x = r;
        x.b[1] = 2;  LogRecord_Seal(&x, 7);
        equ(LogRecord_Check(&x), LOG_REC_BAD, "чужая версия формата — не годна");
        x = r; x.b[0] = 0x00; LogRecord_Seal(&x, 7);
        equ(LogRecord_Check(&x), LOG_REC_BAD, "нулевой тип — не годна");
        memset(x.b, 0xFF, sizeof(x.b));
        equ(LogRecord_Check(&x), LOG_REC_ERASED, "стёртый слот");
        memset(x.b, 0x00, sizeof(x.b));
        equ(LogRecord_Check(&x), LOG_REC_BAD, "нули — не запись");
    }
    /* CRC — стандартный CRC32 (zip): "123456789" -> CBF43926. */
    equ(LogRecord_Crc32((const uint8_t *)"123456789", 9), 0xCBF43926UL, "CRC32 по стандартному вектору");

    /* Неразобранная метка — заметная дыра, а не правдоподобная дата. */
    {
        LogMeas m = CSV_GOLDEN[0].m;
        strcpy(m.ts, "2026-10-22 13:00");
        LogRecord_PackMeas(&r, &m);
        LogRecord_FormatCsv(&r, line, sizeof(line));
        ok(strncmp(line, "xxxx-xx-xx xx:xx:xx,", 20) == 0, "неразобранная метка — заметная дыра");
    }

    /* Текст старого архива. */
    {
        const uint8_t *t = NULL; uint8_t f = 0;
        LogRecord_MakeText(&r, 3, (const uint8_t *)"a,b\r\nc", 6);
        LogRecord_Seal(&r, 1);
        equ(LogRecord_Check(&r), LOG_REC_OK, "TEXT годна");
        equ(LogRecord_Text(&r, &t, &f), 6U, "TEXT: длина");
        ok(memcmp(t, "a,b\r\nc", 6) == 0 && f == 3, "TEXT: байты как есть и номер файла");
        equ((unsigned)LogRecord_FormatCsv(&r, line, sizeof(line)), (unsigned)-1, "TEXT в строку CSV не форматируется");
    }
}

/* -------------------------------------------- модель NOR-флеш на хосте
 *
 * Программирование только сбрасывает биты (1 -> 0), стирание сектора —
 * в 0xFF, счётчик стираний по секторам. Обрыв питания: операция номер
 * fm_cut выполняется частично (ничего / целиком / случайная часть битов) и
 * все дальнейшие операции отказывают, пока тест не «включит питание».
 *
 * Ещё три вида сбоев (приёмка П5а, второй уровень):
 *   — отказ одного чтения: k-го по счёту (fm_read_fail) или по точному
 *     адресу и длине (fm_fail_addr/fm_fail_len) — T1;
 *   — блоки заперты (fm_locked): так SST26 выходит из своего сброса по
 *     питанию. Программирование и стирание «успешны», но ничего не делают,
 *     пока не снята защита (fm_unlock; fm_unlock_fail — сколько раз подряд
 *     снять не удастся). fm_lock_at — запереть перед операцией с этим
 *     номером (сброс одной флеш посреди серии) — T2;
 *   — вид обрыва 4: оборванное стирание или программирование оставляет
 *     ячейки «слабыми» — сейчас они читаются как 0xFF, а после fm_age()
 *     (недели, другая температура) теряют случайные единицы, в том числе
 *     у записанного поверх. Настоящее стирание сектора это лечит — T4. */
#define FM_MAX_SECT  512U
static uint8_t  fm[FM_MAX_SECT * 4096U];
static uint32_t fm_nsect;
static uint32_t fm_erases[FM_MAX_SECT];
static long     fm_ops, fm_cut = -1;
static int      fm_dead, fm_mode, fm_fail_once;
static uint32_t fm_rng = 1;
static int      fm_locked, fm_unlock_fail;
static long     fm_lock_at = -1;
static uint8_t  fm_weak_sect[FM_MAX_SECT];
static uint8_t  fm_weak_slot[FM_MAX_SECT * 64U];

static uint32_t fm_rand(void)
{
    fm_rng ^= fm_rng << 13; fm_rng ^= fm_rng >> 17; fm_rng ^= fm_rng << 5;
    return fm_rng;
}

static void fm_reset(uint32_t nsect)
{
    fm_nsect = nsect;
    memset(fm, 0xFF, nsect * 4096U);
    memset(fm_erases, 0, sizeof(fm_erases));
    memset(fm_weak_sect, 0, sizeof(fm_weak_sect));
    memset(fm_weak_slot, 0, sizeof(fm_weak_slot));
    fm_ops = 0; fm_cut = -1; fm_dead = 0;
    fm_locked = 0; fm_unlock_fail = 0; fm_lock_at = -1;
}

static uint8_t fm_unlock(void)
{
    if (fm_dead) return 0;
    if (fm_unlock_fail > 0) { fm_unlock_fail--; return 0; }
    fm_locked = 0;
    return 1;
}

/* Слабые ячейки теряют единицы: в каждом байте слабого места — с
 * вероятностью 1/4 один бит. */
static void fm_age(void)
{
    for (uint32_t i = 0; i < fm_nsect * 4096U; i++)
        if ((fm_weak_sect[i / 4096U] || fm_weak_slot[i / 64U]) && (fm_rand() & 3U) == 0U)
            fm[i] &= (uint8_t)~(1U << (fm_rand() & 7U));
}

static void fm_power_on(void) { fm_dead = 0; fm_cut = -1; }

static long fm_read_fail = -1;   /* через сколько чтений одно откажет */
static long fm_fail_addr = -1;   /* или: откажет одно чтение ровно (адрес, длина) */
static uint32_t fm_fail_len;

static uint8_t fm_read(uint32_t a, uint8_t *b, uint32_t n)
{
    if (fm_read_fail >= 0 && fm_read_fail-- == 0) return 0;
    if (fm_fail_addr >= 0 && (long)a == fm_fail_addr && n == fm_fail_len) { fm_fail_addr = -1; return 0; }
    if (fm_dead || a + n > fm_nsect * 4096U) return 0;
    memcpy(b, fm + a, n);
    return 1;
}

/* Частичный исход оборванной операции: 0 — не началась, 1 — дошла до конца,
 * 2 — случайная часть битов. */
static int fm_cut_now(void)
{
    if (fm_dead) return -1;
    if (fm_ops == fm_lock_at) { fm_locked = 1; fm_lock_at = -1; }
    if (fm_ops++ != fm_cut) return 0;
    fm_dead = 1;
    return 1;
}

static uint8_t fm_program(uint32_t a, const uint8_t *b, uint32_t n)
{
    int c = fm_cut_now();
    if (c < 0 || a + n > fm_nsect * 4096U || (a / 256U) != ((a + n - 1U) / 256U)) return 0;
    if (fm_locked) return c ? 0 : 1;           /* заперто: «успех», ничего не записано */
    if (c && fm_mode == 4)
    {
        /* Недозаряженные ячейки: читаются стёртыми, но слабые. */
        for (uint32_t i = 0; i < n; i += 64U) fm_weak_slot[(a + i) / 64U] = 1;
        return 0;
    }
    if (fm_fail_once)
    {
        /* Сбой без обрыва питания: 1 — не записалось ничего (шина), 2 —
         * записалось частично (просадка), драйвер вернул отказ. */
        if (fm_fail_once == 2)
            for (uint32_t i = 0; i < n; i++)
                fm[a + i] &= (uint8_t)(b[i] | (uint8_t)fm_rand());
        fm_fail_once = 0;
        return 0;
    }
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t want = fm[a + i] & b[i];
        if (c == 0 || fm_mode == 1)      fm[a + i] = want;
        else if (fm_mode >= 2)           fm[a + i] = want | (fm[a + i] & (uint8_t)fm_rand());
        /* fm_mode 0 при обрыве — ничего не записано */
    }
    return c ? 0 : 1;
}

static uint8_t fm_erase(uint32_t a)
{
    int c = fm_cut_now();
    if (c < 0 || a % 4096U || a >= fm_nsect * 4096U) return 0;
    if (fm_locked) return c ? 0 : 1;
    fm_erases[a / 4096U]++;
    /* Вид 4: стирание оборвано, сектор читается чистым, но ячейки слабые.
     * Дошедшее до конца стирание слабость снимает. */
    fm_weak_sect[a / 4096U] = (uint8_t)(c && fm_mode == 4);
    if (!c || fm_mode == 4)
        memset(fm_weak_slot + a / 64U, 0, 64U);
    /* Вид 3: стирание оборвано так, что часть записей уцелела целиком, а
     * часть — в мусоре (у вида 0 уцелели все, у вида 2 — ни одной). */
    uint8_t keep = 0;
    for (uint32_t i = 0; i < 4096U; i++)
    {
        if (i % 64U == 0U)
            keep = (uint8_t)(fm_rand() & 1U);
        if (c == 0 || fm_mode == 1 || fm_mode == 4) fm[a + i] = 0xFF;
        else if (fm_mode == 2)               fm[a + i] |= (uint8_t)fm_rand();
        else if (fm_mode == 3 && !keep)      fm[a + i] |= (uint8_t)fm_rand();
    }
    return c ? 0 : 1;
}

static const LogFlashOps FM_OPS = { fm_read, fm_program, fm_erase, NULL, fm_unlock };

/* Сектор за сектором записи стёрт целиком — «минимум один стёртый впереди». */
static int fm_spare_blank(const LogRing *r)
{
    uint32_t s = (r->w_sect + 1U) % r->nsect;
    for (uint32_t i = 0; i < 4096U; i++)
        if (fm[s * 4096U + i] != 0xFF) return 0;
    return 1;
}

/* Запись-замер с содержимым, однозначно выведенным из id: по ней видно,
 * что прочиталось именно то, что писалось, а не чужие данные. */
static void log_make(LogRecord *r, uint32_t id)
{
    LogMeas m;
    memset(&m, 0, sizeof(m));
    snprintf(m.ts, sizeof(m.ts), "20%02u-%02u-%02u %02u:%02u:%02u",
             (unsigned)(id % 100U), (unsigned)(id % 12U + 1U), (unsigned)(id % 28U + 1U),
             (unsigned)(id % 24U), (unsigned)(id % 60U), (unsigned)((id / 60U) % 60U));
    m.air_temp = (float)id * 0.01f;
    m.boot_count = id;
    m.write_fails = id * 2654435761UL;
    m.lowpower_skips = ~id;
    LogRecord_PackMeas(r, &m);
}

static uint32_t log_id(const LogRecord *r)
{
    LogMeas m;
    LogRecord_UnpackMeas(r, &m);
    return m.boot_count;
}

static uint8_t log_is(const LogRecord *r, uint32_t id)
{
    LogRecord want;
    log_make(&want, id);
    /* Всё, кроме номера и CRC, — как у записи id. */
    return memcmp(want.b, r->b, 2) == 0 &&
           memcmp(want.b + LOG_PAYLOAD_OFS, r->b + LOG_PAYLOAD_OFS, LOG_PAYLOAD_SIZE) == 0;
}

/* Прочитать весь журнал: годные записи по порядку должны идти с растущими
 * id и совпадать с тем, что писалось. Возвращает число годных; *bad —
 * испорченных слотов; ids — id годных (если не NULL). */
static uint32_t log_scan_skipped;   /* из *bad — стёртых слотов (пропуск после старта, T4) */

static uint32_t log_scan(const LogRing *r, uint32_t *bad, uint32_t *ids, uint32_t max, int *alien)
{
    LogRecord rec;
    uint32_t n = LogRing_Count(r), good = 0, prev = 0;
    *bad = 0; *alien = 0;
    log_scan_skipped = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t st = LogRing_Read(r, i, &rec);
        if (st == LOG_READ_BAD) { (*bad)++; log_scan_skipped += LogRecord_IsErased(&rec); continue; }
        if (st != LOG_READ_OK) { (*alien)++; continue; }
        uint32_t id = log_id(&rec);
        if (!log_is(&rec, id) || (good && id <= prev)) (*alien)++;
        if (ids && good < max) ids[good] = id;
        prev = id;
        good++;
    }
    return good;
}

/* -------------------------------------------------------- G1: по кругу */
static void test_log_ring_g1(void)
{
    LogRing r;
    LogRecord rec;
    const uint32_t NS = 8, LAPS = 12;

    puts("Журнал G1: запись не останавливается от заполнения");

    fm_reset(NS);
    equ(LogRing_Mount(&r, &FM_OPS, NS), LOG_MOUNT_EMPTY, "чистая флеш — журнала нет");
    equ(LogRing_Count(&r), 0U, "пуст");

    uint32_t total = NS * LOG_SLOTS * LAPS, fails = 0, order_bad = 0, drop_bad = 0, spare_bad = 0;
    uint32_t min_full = 0xFFFFFFFFUL, max_cnt = 0, prev_first = 0;
    for (uint32_t id = 1; id <= total; id++)
    {
        log_make(&rec, id);
        if (!LogRing_Append(&r, &rec)) fails++;
        if (!fm_spare_blank(&r)) spare_bad++;
        uint32_t cnt = LogRing_Count(&r), first = LogRing_FirstSeq(&r);
        if (cnt > max_cnt) max_cnt = cnt;
        if (id > NS * LOG_SLOTS && cnt < min_full) min_full = cnt;
        /* Старейшие уходят только целым сектором. */
        if (first != prev_first && (first - prev_first != LOG_SLOTS || first % LOG_SLOTS)) drop_bad++;
        prev_first = first;
        /* Самая новая запись — только что записанная. */
        LogRecord nw;
        if (LogRing_Newest(&r, &nw, NULL) != LOG_READ_OK || !log_is(&nw, id)) order_bad++;
        if (id % (LOG_SLOTS * 3U) == 0U)
        {
            /* Порядок и непрерывность: годные идут подряд, без пропусков. */
            static uint32_t ids[FM_MAX_SECT * 64U];
            uint32_t bad; int alien;
            uint32_t good = log_scan(&r, &bad, ids, sizeof(ids) / 4U, &alien);
            if (bad || alien || good != cnt || ids[good - 1U] != id || ids[0] != id - good + 1U)
                order_bad++;
            /* После перезапуска — тот же журнал. */
            LogRing m;
            if (LogRing_Mount(&m, &FM_OPS, NS) != LOG_MOUNT_OK || LogRing_Count(&m) != cnt ||
                LogRing_FirstSeq(&m) != first)
                order_bad++;
        }
    }
    equ(fails, 0U, "все записи прошли — 12 кругов кольца");
    equ(order_bad, 0U, "порядок, непрерывность и перемонтирование");
    equ(drop_bad, 0U, "старейшие уходят посекторно");
    equ(spare_bad, 0U, "после каждой записи сектор за сектором записи стёрт");
    /* Сектор записи и запасной не читаются, пока пусты: в журнале от (N-2)
     * полных секторов (сразу после перехода в новый) до (N-1) без одной. */
    equ(max_cnt, (NS - 1U) * LOG_SLOTS - 1U, "в журнале не больше (N-1) секторов без одной записи");
    equ(min_full, (NS - 2U) * LOG_SLOTS, "и не меньше (N-2) полных секторов");
    equ(r.next_seq, total, "номер растёт на 1 на запись");

    /* Полный размер чипа: 3 круга по 32768 записей (12 кругов — выше, на
     * малом кольце той же логики). */
    fm_reset(LOG_FLASH_SECTORS);
    (void)LogRing_Mount(&r, &FM_OPS, LOG_FLASH_SECTORS);
    fails = 0;
    total = LOG_FLASH_SECTORS * LOG_SLOTS * 3U;
    for (uint32_t id = 1; id <= total; id++)
    {
        log_make(&rec, id);
        if (!LogRing_Append(&r, &rec)) fails++;
    }
    {
        uint32_t bad; int alien;
        uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
        LogRecord first;
        equ(fails, 0U, "2 МБ: 3 круга без отказов");
        equ(good, LogRing_Count(&r), "2 МБ: все записи журнала годны");
        ok(good >= (LOG_FLASH_SECTORS - 2U) * LOG_SLOTS, "2 МБ: в журнале не меньше 510 секторов записей");
        ok(!bad && !alien, "2 МБ: ни испорченных, ни чужих");
        ok(LogRing_Read(&r, 0, &first) == LOG_READ_OK && log_is(&first, total - good + 1U),
           "2 МБ: старейшая — ровно та, что ожидалась");
        LogRing m;
        equ(LogRing_Mount(&m, &FM_OPS, LOG_FLASH_SECTORS), LOG_MOUNT_OK, "2 МБ: перемонтирование");
        ok(LogRing_Count(&m) == LogRing_Count(&r) && LogRing_FirstSeq(&m) == LogRing_FirstSeq(&r),
           "2 МБ: после перезапуска тот же журнал");
    }
}

/* -------------------------------------------------- G4: износ по секторам */
static void test_log_ring_g4(void)
{
    LogRing r;
    LogRecord rec;
    const uint32_t NS = 16, LAPS = 20;

    puts("Журнал G4: равномерный износ");

    fm_reset(NS);
    (void)LogRing_Mount(&r, &FM_OPS, NS);
    for (uint32_t id = 1; id <= NS * LOG_SLOTS * LAPS; id++)
    {
        log_make(&rec, id);
        (void)LogRing_Append(&r, &rec);
    }
    uint32_t mn = 0xFFFFFFFFUL, mx = 0;
    for (uint32_t s = 0; s < NS; s++)
    {
        if (fm_erases[s] < mn) mn = fm_erases[s];
        if (fm_erases[s] > mx) mx = fm_erases[s];
    }
    ok(mx - mn <= 1U, "20 кругов: стирания по секторам расходятся не больше чем на 1");
    ok(mx <= LAPS + 1U, "сектор стирается один раз за круг");

    /* С перезапусками каждые 37 записей. После монтирования запасной (и
     * сектор записи, если пишется с его слота 0) стирается безусловно (T4):
     * не больше двух лишних стираний на старт, и они расходятся по кругу. */
    fm_reset(NS);
    (void)LogRing_Mount(&r, &FM_OPS, NS);
    uint32_t restarts = 0, tot = 0;
    for (uint32_t id = 1; id <= NS * LOG_SLOTS * LAPS; id++)
    {
        log_make(&rec, id);
        (void)LogRing_Append(&r, &rec);
        if (id % 37U == 0U)
        {
            (void)LogRing_Mount(&r, &FM_OPS, NS);
            restarts++;
        }
    }
    mn = 0xFFFFFFFFUL; mx = 0;
    for (uint32_t s = 0; s < NS; s++)
    {
        tot += fm_erases[s];
        if (fm_erases[s] < mn) mn = fm_erases[s];
        if (fm_erases[s] > mx) mx = fm_erases[s];
    }
    ok(mx - mn <= 4U && tot <= NS * LAPS + 1U + 2U * restarts,
       "с перезапусками: износ ровный, лишних стираний не больше двух на старт");
}

/* ------------------------------------------------------ G2: обрыв питания
 *
 * Прогон: пишем записи с id 1, 2, ...; на операции номер cut питание
 * пропадает. Включаем, монтируем и проверяем:
 *   — ни одной записи с чужим содержимым и ни одной не на своём месте;
 *   — все подтверждённые записи, что моложе нового хвоста, на месте;
 *   — хвост ушёл не дальше чем на сектор против состояния до обрыва
 *     (оборвано стирание старейшего — его записи и пропали);
 *   — испорченных слотов в журнале не больше одного (тот, что писался);
 *   — после восстановления журнал пишется дальше и читается. */
static int log_g2_run(uint32_t ns, uint32_t total, long cut, int mode, int verbose)
{
    LogRing r;
    LogRecord rec;
    static uint8_t acked[FM_MAX_SECT * 64U * 4U];
    uint32_t last_ok = 0, first_before = 0;
    int errors = 0;

    fm_reset(ns);
    fm_mode = mode;
    fm_cut = cut;
    (void)LogRing_Mount(&r, &FM_OPS, ns);
    memset(acked, 0, total + 2U);
    uint32_t id;
    for (id = 1; id <= total && !fm_dead; id++)
    {
        first_before = LogRing_FirstSeq(&r);
        log_make(&rec, id);
        if (LogRing_Append(&r, &rec)) { acked[id] = 1; last_ok = id; }
    }
    if (!fm_dead)
        return -1;   /* обрыв не случился — точек больше нет */
    uint32_t inflight = id - 1U;

    fm_power_on();
    LogRing m;
    uint8_t mr = LogRing_Mount(&m, &FM_OPS, ns);
    /* Ни одной подтверждённой записи — журнала могло и не остаться: пусто
     * или единственный недописанный слот (чужое; LogArchive_Open начнёт
     * журнал с нуля). */
    if (mr != LOG_MOUNT_OK && !(mr != LOG_MOUNT_EIO && last_ok == 0U))
    {
        if (verbose) printf("    cut %ld: mount %u\n", cut, mr);
        return 1;
    }

    static uint32_t ids[FM_MAX_SECT * 64U];
    uint32_t bad; int alien;
    uint32_t good = log_scan(&m, &bad, ids, sizeof(ids) / 4U, &alien);
    if (alien) errors++;
    if (bad > 1U) errors++;
    /* Хвост: не дальше чем на сектор вперёд против состояния до обрыва. */
    if ((int32_t)(LogRing_FirstSeq(&m) - first_before) > (int32_t)LOG_SLOTS) errors++;
    /* Подтверждённые моложе старейшей прочитанной — все на месте. */
    uint32_t oldest = good ? ids[0] : last_ok + 1U;
    uint32_t j = 0, missing = 0;
    for (uint32_t k = oldest; k <= last_ok; k++)
    {
        if (!acked[k]) continue;
        while (j < good && ids[j] < k) j++;
        if (j >= good || ids[j] != k) missing++;
    }
    if (missing) errors++;
    /* Лишнего нет: годная — подтверждённая или та, что писалась. */
    for (uint32_t k = 0; k < good; k++)
        if (!acked[ids[k]] && ids[k] != inflight) errors++;
    /* Подтверждённых из прежнего журнала пропало не больше сектора. */
    uint32_t lost = 0;
    for (uint32_t k = 1; k <= last_ok; k++)
        if (acked[k] && k < oldest && k + (ns - 2U) * LOG_SLOTS > last_ok) lost++;
    if (lost > LOG_SLOTS) errors++;

    /* Жизнь продолжается: ещё полтора круга. */
    uint32_t more_fail = 0;
    for (uint32_t k = 0; k < ns * LOG_SLOTS * 3U / 2U; k++)
    {
        log_make(&rec, total + 10U + k);
        if (!LogRing_Append(&m, &rec)) more_fail++;
        if (!fm_spare_blank(&m)) more_fail++;
    }
    good = log_scan(&m, &bad, NULL, 0, &alien);
    if (more_fail || alien || bad || good != LogRing_Count(&m)) errors++;

    if (errors && verbose)
        printf("    cut %ld mode %d: alien %d bad %u missing %u lost %u more_fail %u\n",
               cut, mode, alien, bad, missing, lost, more_fail);
    return errors ? 1 : 0;
}

static void test_log_ring_g2(void)
{
    puts("Журнал G2: обрыв питания после каждой операции");

    /* 5 секторов, 2.2 круга: обрыв на каждой операции программирования и
     * стирания, в четырёх видах обрыва. */
    for (int mode = 0; mode < 4; mode++)
    {
        uint32_t points = 0, failed = 0;
        for (long cut = 0;; cut++)
        {
            fm_rng = 0x9E3779B9U + (uint32_t)cut;
            int res = log_g2_run(5, 5U * LOG_SLOTS * 11U / 5U, cut, mode, failed < 3U);
            if (res < 0) break;
            points++;
            failed += (uint32_t)res;
        }
        char what[200];
        snprintf(what, sizeof(what), "обрыв вида %d: %u точек, после каждой — не больше одной потери",
                 mode, points);
        ok(points > 700U && failed == 0U, what);
    }

    /* Обрыв во время восстановления: второй обрыв на первой же операции
     * после включения (стирание запасного), потом ещё один. */
    {
        uint32_t failed = 0;
        LogRing r;
        LogRecord rec;
        fm_reset(6);
        (void)LogRing_Mount(&r, &FM_OPS, 6);
        uint32_t id = 0, last_ok = 0;
        for (uint32_t round = 0; round < 400U; round++)
        {
            fm_mode = (int)(round % 4U);
            fm_cut = fm_ops + (long)(fm_rand() % 90U);
            while (!fm_dead)
            {
                log_make(&rec, ++id);
                if (LogRing_Append(&r, &rec)) last_ok = id;
                else if (!fm_dead) failed++;   /* отказ без обрыва — восстановление не доделано */
            }
            fm_power_on();
            if (LogRing_Mount(&r, &FM_OPS, 6) != LOG_MOUNT_OK) { failed++; continue; }
            uint32_t bad; int alien;
            static uint32_t ids[6 * 64];
            uint32_t good = log_scan(&r, &bad, ids, 6 * 64, &alien);
            if (alien) failed++;
            /* Последняя подтверждённая — на месте (сколько бы ни было обрывов). */
            if (!good || ids[good - 1U] < last_ok) failed++;
        }
        equ(failed, 0U, "400 обрывов подряд в случайных местах (и во время восстановления): чужих нет, "
                        "последняя подтверждённая цела, запись после включения идёт");
    }

    /* Сбой программирования без обрыва (просадка: записалось частично):
     * запись уходит в следующий слот, испорченный пропускается номером. */
    {
        LogRing r;
        LogRecord rec, got;
        fm_reset(4);
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        for (uint32_t id = 1; id <= 10U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        fm_fail_once = 2;
        log_make(&rec, 11);
        equ(LogRing_Append(&r, &rec), 1, "сбой программирования — повтор в следующий слот удался");
        equ(LogRing_Count(&r), 12U, "испорченный слот занял один номер");
        ok(LogRing_Read(&r, 10, &got) == LOG_READ_BAD && LogRing_Read(&r, 11, &got) == LOG_READ_OK &&
           log_is(&got, 11), "замер 11 — в следующем слоте");
    }

    /* Сбой в слоте 0 сектора — ничего не записалось и записалось частично.
     * Записи за этим слотом не должны пропасть при перезапуске: сектор со
     * стёртым или испорченным слотом 0 восстановление не сочтёт пустым. */
    for (int how = 1; how <= 2; how++)
    {
        LogRing r, m;
        LogRecord rec;
        fm_reset(4);
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        uint32_t id;
        for (id = 1; id <= 64U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        fm_fail_once = how;
        for (; id <= 64U + 20U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        equ(LogRing_Mount(&m, &FM_OPS, 4), LOG_MOUNT_OK, how == 1 ? "сбой в слоте 0 (пусто): перемонтирование"
                                                                  : "сбой в слоте 0 (частично): перемонтирование");
        uint32_t bad; int alien;
        static uint32_t ids[4 * 64];
        uint32_t good = log_scan(&m, &bad, ids, 4 * 64, &alien);
        ok(good == 84U && !alien && ids[83] == 84U && bad == (how == 2 ? 1U : 0U),
           how == 1 ? "сбой в слоте 0 (пусто): все 84 записи на месте, слот не потрачен"
                    : "сбой в слоте 0 (частично): все 84 записи на месте, один слот испорчен");
        for (id = 85; id <= 90U; id++) { log_make(&rec, id); (void)LogRing_Append(&m, &rec); }
        good = log_scan(&m, &bad, ids, 4 * 64, &alien);
        ok(good == 90U && ids[89] == 90U && !alien, "после перезапуска пишется дальше");
    }

    /* Сбой программирования слота 0 и тут же сбой чтения сверки: что легло
     * в слот, неизвестно. Журнал закрывается без повтора, после
     * переоткрытия ни одна подтверждённая запись не пропадает. */
    {
        LogRing r, m;
        LogRecord rec;
        fm_reset(4);
        fm_mode = 1;
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        uint32_t id;
        for (id = 1; id <= 64U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        fm_fail_once = 1;
        fm_read_fail = 0;
        log_make(&rec, 65);
        equ(LogRing_Append(&r, &rec), 0, "сбой записи и чтения сверки — отказ без повтора");
        fm_read_fail = -1;
        equ(LogRing_Mount(&m, &FM_OPS, 4), LOG_MOUNT_OK, "переоткрытие");
        for (id = 66; id <= 64U + 40U; id++) { log_make(&rec, id); (void)LogRing_Append(&m, &rec); }
        equ(LogRing_Mount(&m, &FM_OPS, 4), LOG_MOUNT_OK, "перезапуск");
        uint32_t bad; int alien;
        static uint32_t ids[4 * 64];
        uint32_t good = log_scan(&m, &bad, ids, 4 * 64, &alien);
        ok(good == 64U + 39U && ids[good - 1U] == 104U && !alien, "все подтверждённые на месте");
    }

    /* Годный сектор журнала с несмежным номером (остаток старого круга, не
     * стёртый из-за отказа) в журнал не пристёгивается, а годная запись не на
     * своём месте не читается как своя. */
    {
        LogRing r, m;
        LogRecord rec;
        fm_reset(8);
        (void)LogRing_Mount(&r, &FM_OPS, 8);
        for (uint32_t id = 1; id <= 3U * 64U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        /* Сектор 7 (не запасной): записи с номерами 5.. — «чужой круг». */
        for (uint32_t k = 0; k < 64U; k++)
        {
            log_make(&rec, 900U + k);
            LogRecord_Seal(&rec, 5U + k);
            memcpy(fm + 7U * 4096U + k * 64U, rec.b, 64);
        }
        equ(LogRing_Mount(&m, &FM_OPS, 8), LOG_MOUNT_OK, "сектор с несмежным номером: монтирование");
        equ(LogRing_FirstSeq(&m), 0U, "сектор с несмежным номером к хвосту не пристёгнут");
        equ(LogRing_Count(&m), 3U * 64U, "в журнале только свои записи");
        /* В слоте 10 сектора 1 — годная запись номер 3 из сектора 0, как
         * будто стирание сектора тихо не прошло. */
        fm_reset(4);
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        for (uint32_t id = 1; id <= 100U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        memcpy(fm + 1U * 4096U + 10U * 64U, fm + 3U * 64U, 64);
        LogRecord got;
        ok(LogRing_Read(&r, 64U + 10U, &got) == LOG_READ_BAD, "годная запись не на своём месте — не своя");
    }

    /* Двойной обрыв: недописан слот 0 нового сектора, а при восстановлении
     * оборвано стирание этого сектора (мусор по всему сектору). Следующий
     * старт обязан стереть его снова, а не писать по мусору. */
    {
        LogRing r;
        LogRecord rec;
        fm_reset(4);
        fm_mode = 2;
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        for (uint32_t id = 1; id <= 64U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        fm_cut = fm_ops;                 /* программирование слота 0 сектора 1 */
        log_make(&rec, 65); (void)LogRing_Append(&r, &rec);
        fm_power_on();
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        fm_cut = fm_ops;                 /* стирание сектора 1 при восстановлении */
        log_make(&rec, 66); (void)LogRing_Append(&r, &rec);
        fm_power_on();
        equ(LogRing_Mount(&r, &FM_OPS, 4), LOG_MOUNT_OK, "двойной обрыв: монтирование");
        uint32_t fails = 0;
        for (uint32_t id = 67; id <= 80U; id++) { log_make(&rec, id); fails += !LogRing_Append(&r, &rec); }
        uint32_t bad; int alien;
        uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
        ok(!fails && good == 64U + 14U && !alien && bad == 0U,
           "двойной обрыв: сектор стёрт заново (мусор слота 0 не остался), запись идёт");
    }

    /* Полный чип: 8 случайных обрывов на трёх кругах. */
    {
        uint32_t failed = 0;
        for (int k = 0; k < 8; k++)
        {
            fm_rng = 12345U + (uint32_t)k * 7919U;
            long cut = (long)(fm_rand() % (LOG_FLASH_SECTORS * LOG_SLOTS * 3U + 1500U));
            failed += (uint32_t)(log_g2_run(LOG_FLASH_SECTORS, LOG_FLASH_SECTORS * LOG_SLOTS * 3U,
                                            cut, k % 3, 1) != 0);
        }
        equ(failed, 0U, "2 МБ: 8 случайных обрывов на трёх кругах");
    }
}

/* ------------------------------------- приёмка П5а, второй уровень: T2–T4 */

/* Вид обрыва 4 (слабые ячейки): обрыв на операции cut, включение, ещё два
 * сектора записей, «старение» и перезапуск. Все подтверждённые записи моложе
 * хвоста на месте, чужих нет, испорченных — не больше одной (та, что
 * писалась при обрыве), остальное — стёртые пропуски после старта. */
static int log_weak_run(long cut, int verbose)
{
    LogRing r, m;
    LogRecord rec;
    static uint8_t acked[5U * 64U * 4U + 400U];
    const uint32_t ns = 5, total = 5U * LOG_SLOTS * 11U / 5U;
    fm_reset(ns);
    fm_mode = 4;
    fm_cut = cut;
    memset(acked, 0, sizeof(acked));
    (void)LogRing_Mount(&r, &FM_OPS, ns);
    uint32_t id;
    for (id = 1; id <= total && !fm_dead; id++)
    {
        log_make(&rec, id);
        if (LogRing_Append(&r, &rec)) acked[id] = 1;
    }
    if (!fm_dead)
        return -1;
    fm_power_on();
    uint8_t mr = LogRing_Mount(&m, &FM_OPS, ns);
    if (mr == LOG_MOUNT_EIO) return 1;
    uint32_t last = total + 2U * LOG_SLOTS + 10U;
    for (id = total + 1U; id <= last; id++)
    {
        log_make(&rec, id);
        if (LogRing_Append(&m, &rec)) acked[id] = 1;
    }
    fm_age();
    if (LogRing_Mount(&m, &FM_OPS, ns) != LOG_MOUNT_OK) return 1;
    static uint32_t ids[5 * 64];
    uint32_t bad; int alien;
    uint32_t good = log_scan(&m, &bad, ids, 5 * 64, &alien);
    uint32_t j = 0, missing = 0;
    for (uint32_t k = good ? ids[0] : 1U; k <= last; k++)
    {
        if (!acked[k]) continue;
        while (j < good && ids[j] < k) j++;
        if (j >= good || ids[j] != k) missing++;
    }
    int e = (alien || missing || bad - log_scan_skipped > 1U || !good || ids[good - 1U] != last);
    if (e && verbose)
        printf("    вид 4, обрыв %ld: alien %d missing %u bad %u (пропусков %u)\n",
               cut, alien, missing, bad, log_scan_skipped);
    return e;
}

static void test_log_ring_accept2(void)
{
    LogRing r, m;
    LogRecord rec;
    puts("Журнал: приёмка второго уровня (T2–T4)");

    /* T2: сброс одной флеш посреди работы — блоки заперты, запись идёт в
     * том же вызове: защита снимается перед каждой серией. */
    {
        fm_reset(8);
        fm_mode = 1;
        (void)LogRing_Mount(&r, &FM_OPS, 8);
        uint32_t okc = 0, id;
        for (id = 1; id <= 100U; id++) { log_make(&rec, id); okc += LogRing_Append(&r, &rec); }
        for (uint32_t cyc = 0; cyc < 500U; cyc++)
        {
            if (cyc % 7U == 0U) fm_locked = 1;
            log_make(&rec, id++);
            okc += LogRing_Append(&r, &rec);
        }
        uint32_t bad; int alien;
        uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
        ok(okc == 600U && !alien && !bad && good == LogRing_Count(&r),
           "T2: флеш запирается каждые 7 записей — все 500 записей прошли, журнал читается");
    }

    /* T2: сброс флеш перед каждой операцией серии по очереди (в т.ч. перед
     * стиранием запасного, в котором лежат старейшие записи: кольцо уже
     * прошло круг): ни одного отказа, ни одной испорченной записи. */
    {
        uint32_t fails = 0, broken = 0, points = 0;
        for (long at = 0; at < 190; at++)
        {
            fm_reset(8);
            fm_mode = 1;
            (void)LogRing_Mount(&r, &FM_OPS, 8);
            uint32_t id;
            for (id = 1; id <= 8U * 64U + 20U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
            fm_lock_at = fm_ops + at;
            for (uint32_t k = 0; k < 3U * 64U; k++, id++)
            {
                log_make(&rec, id);
                fails += !LogRing_Append(&r, &rec);
            }
            points += (fm_lock_at < 0);
            uint32_t bad; int alien;
            uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
            broken += (alien || bad || good != LogRing_Count(&r) || good < 5U * 64U);
        }
        ok(points == 190U && fails == 0U && broken == 0U,
           "T2: флеш заперта перед любой из 190 операций — запись возобновилась в том же вызове");
    }

    /* T2: защита не снимается — отказ без порчи и без траты слота; после
     * переоткрытия (main переинициализирует флеш) запись идёт дальше. */
    {
        fm_reset(4);
        fm_mode = 1;
        (void)LogRing_Mount(&r, &FM_OPS, 4);
        for (uint32_t id = 1; id <= 70U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        fm_locked = 1;
        fm_unlock_fail = 2;
        log_make(&rec, 71);
        uint8_t first = LogRing_Append(&r, &rec);
        static LogOldVolume v;
        equ(LogArchive_Open(&m, &FM_OPS, 4, 1, &v), LOG_OPEN_OK, "T2: переоткрытие после отказа");
        equ(LogRing_Append(&m, &rec), 1, "T2: после переоткрытия запись прошла");
        uint32_t bad; int alien;
        uint32_t good = log_scan(&m, &bad, NULL, 0, &alien);
        ok(first == 0 && good == 71U && !alien && bad == log_scan_skipped,
           "T2: защита не снялась — отказ, потом запись; подтверждённые целы");
    }

    /* T3: один сброшенный бит в байтах 0..5 слота 0 — в середине кольца и в
     * голове. Журнал не укорачивается, голова не стирается. */
    {
        uint32_t failed = 0, cases = 0;
        for (int where = 0; where < 2; where++)
            for (uint32_t byte = 0; byte < 6U; byte++)
                for (uint32_t bit = 0; bit < 8U; bit++)
                {
                    fm_reset(64);
                    fm_mode = 1;
                    (void)LogRing_Mount(&r, &FM_OPS, 64);
                    const uint32_t N = 40U * 64U + 30U;
                    for (uint32_t id = 1; id <= N; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
                    uint32_t s = where ? r.w_sect : 20U;
                    uint32_t cnt = LogRing_Count(&r);
                    uint8_t *b = &fm[s * 4096U + byte];
                    if (!(*b & (1U << bit))) continue;
                    *b &= (uint8_t)~(1U << bit);
                    cases++;
                    int e = 0;
                    if (LogRing_Mount(&m, &FM_OPS, 64) != LOG_MOUNT_OK || LogRing_Count(&m) != cnt) e++;
                    log_make(&rec, N + 1U);
                    if (!LogRing_Append(&m, &rec)) e++;
                    if (LogRing_Mount(&m, &FM_OPS, 64) != LOG_MOUNT_OK) e++;
                    uint32_t bad; int alien;
                    uint32_t good = log_scan(&m, &bad, NULL, 0, &alien);
                    if (alien || good != N || bad != 1U + log_scan_skipped) e++;
                    if (e && failed < 3U)
                        printf("    %s, байт %u бит %u: count %u good %u bad %u\n", where ? "голова" : "середина",
                               byte, bit, LogRing_Count(&m), good, bad);
                    failed += (e != 0);
                }
        char what[200];
        snprintf(what, sizeof(what), "T3: бит 1→0 в байтах 0..5 слота 0 (%u случаев, середина и голова) — "
                                     "журнал цел, потерян только этот слот", cases);
        ok(cases >= 10U && failed == 0U, what);
    }

    /* T3: сектор посреди журнала испорчен целиком (ни одного годного
     * номера) — журнал не рвётся на нём, старшие записи остаются. */
    {
        fm_reset(16);
        fm_mode = 1;
        (void)LogRing_Mount(&r, &FM_OPS, 16);
        for (uint32_t id = 1; id <= 10U * 64U; id++) { log_make(&rec, id); (void)LogRing_Append(&r, &rec); }
        memset(fm + 4U * 4096U, 0x00, 4096U);
        equ(LogRing_Mount(&m, &FM_OPS, 16), LOG_MOUNT_OK, "T3: сектор-дыра: монтирование");
        uint32_t bad; int alien;
        uint32_t good = log_scan(&m, &bad, NULL, 0, &alien);
        ok(LogRing_FirstSeq(&m) == 0U && good == 9U * 64U && bad == 64U && !alien,
           "T3: сектор-дыра посреди журнала — выпали только его 64 записи");
    }

    /* T4: обрыв вида 4 (слабые ячейки) на каждой операции. */
    {
        uint32_t points = 0, failed = 0;
        for (long cut = 0;; cut++)
        {
            fm_rng = 0xC0FFEEU + (uint32_t)cut;
            int res = log_weak_run(cut, failed < 3U);
            if (res < 0) break;
            points++;
            failed += (uint32_t)res;
        }
        char what[200];
        snprintf(what, sizeof(what), "T4: слабые ячейки после обрыва (%u точек): после «старения» "
                                     "подтверждённые целы", points);
        ok(points > 700U && failed == 0U, what);
    }
    fm_mode = 1;
}

/* ------------------------------------------ импорт старого архива (FAT)
 *
 * Старый том строится в модели флеш так же, как его клал f_mkfs прошлых
 * прошивок: MBR в секторе 0 (запись раздела MKFS_PTE*), раздел с LBA 63,
 * загрузочный сектор — MKFS_BOOT* байт в байт (одна FAT, корень на 512
 * записей, 8129 секторов; FAT16 — кластер 512 Б, FAT12 — 1 КБ). Файлы —
 * строки CSV с \r\n, как их писал f_puts. */
typedef struct {
    const char *name83;     /* 11 символов каталога: "DATA_1  TXT" */
    uint32_t size;
    uint32_t seed;
} OldFileSpec;

static void old_text(uint8_t *out, uint32_t size, uint32_t seed)
{
    uint32_t n = 0, line = 0;
    while (n < size)
    {
        char buf[96];
        int l = (line == 0U)
              ? snprintf(buf, sizeof(buf), "timestamp,air_temp,air_hum,seed%u\r\n", (unsigned)seed)
              : snprintf(buf, sizeof(buf), "2026-%02u-%02u %02u:00:00,%u.%02u,%u\r\n",
                         (unsigned)(seed % 12U + 1U), (unsigned)(line % 28U + 1U), (unsigned)(line % 24U),
                         (unsigned)(line * 7U % 40U), (unsigned)(seed * line % 100U), (unsigned)line);
        for (int i = 0; i < l && n < size; i++)
            out[n++] = (uint8_t)buf[i];
        line++;
    }
}

static void fat_set(uint32_t fat_addr, uint32_t bits, uint32_t c, uint32_t v)
{
    if (bits == 16U)
    {
        fm[fat_addr + c * 2U] = (uint8_t)v;
        fm[fat_addr + c * 2U + 1U] = (uint8_t)(v >> 8);
        return;
    }
    uint32_t o = fat_addr + c + c / 2U;
    if (c & 1U)
    {
        fm[o] = (uint8_t)((fm[o] & 0x0FU) | ((v << 4) & 0xF0U));
        fm[o + 1U] = (uint8_t)(v >> 4);
    }
    else
    {
        fm[o] = (uint8_t)v;
        fm[o + 1U] = (uint8_t)((fm[o + 1U] & 0xF0U) | ((v >> 8) & 0x0FU));
    }
}

/* interleave — кластеры файлов чередуются (фрагментация), иначе подряд.
 * Возвращает байт за последним занятым кластером. */
static uint32_t old_volume(uint32_t fatbits, const OldFileSpec *files, uint32_t nfiles,
                           int interleave, uint8_t texts[][40000])
{
    const unsigned char *tmpl = (fatbits == 16U) ? MKFS_BOOT16 : MKFS_BOOT12;
    uint32_t tot  = 8129U, vol = 63U, rootsz = 32U;
    uint32_t spc  = tmpl[13], fatsz = tmpl[22];
    uint32_t csize = spc * 512U;
    uint32_t fat_addr = (vol + 1U) * 512U;
    uint32_t root_addr = fat_addr + fatsz * 512U;
    uint32_t data_lba = vol + 1U + fatsz + rootsz;

    memset(fm, 0, 64U * 512U + (1U + fatsz + rootsz) * 512U);   /* f_mkfs обнуляет служебное */
    /* MBR; тип FAT16 прошивка правила на 0x0E (Android). */
    memcpy(fm + 446, (fatbits == 16U) ? MKFS_PTE16 : MKFS_PTE12, 16);
    if (fatbits == 16U) fm[446 + 4] = 0x0E;
    fm[510] = 0x55; fm[511] = 0xAA;
    uint8_t *b = fm + vol * 512U;
    memcpy(b, tmpl, 64);
    b[510] = 0x55; b[511] = 0xAA;
    (void)tot;
    fat_set(fat_addr, fatbits, 0, (fatbits == 16U) ? 0xFFF8U : 0xFF8U);
    fat_set(fat_addr, fatbits, 1, (fatbits == 16U) ? 0xFFFFU : 0xFFFU);

    uint32_t need[16], prev[16], first[16], done[16];
    for (uint32_t i = 0; i < nfiles; i++)
    {
        need[i] = (files[i].size + csize - 1U) / csize;
        prev[i] = 0; first[i] = 0; done[i] = 0;
        old_text(texts[i], files[i].size, files[i].seed);
    }
    uint32_t c = 2, end = 0;
    for (uint32_t pass = 0, left = 1; left; pass++)
    {
        left = 0;
        for (uint32_t i = 0; i < nfiles; i++)
        {
            uint32_t take = interleave ? 1U : need[i];
            for (uint32_t t = 0; t < take && done[i] < need[i]; t++)
            {
                if (!first[i]) first[i] = c; else fat_set(fat_addr, fatbits, prev[i], c);
                uint32_t a = (data_lba + (c - 2U) * spc) * 512U, part = files[i].size - done[i] * csize;
                if (part > csize) part = csize;
                memcpy(fm + a, texts[i] + done[i] * csize, part);
                if (a + csize > end) end = a + csize;
                prev[i] = c++;
                done[i]++;
            }
            if (done[i] < need[i]) left = 1;
            else if (prev[i]) fat_set(fat_addr, fatbits, prev[i], (fatbits == 16U) ? 0xFFFFU : 0xFFFU);
        }
    }
    /* Каталог: метка тома и удалённый файл — мимо, наши — по списку. */
    uint8_t *de = fm + root_addr;
    memcpy(de, "FIELDSENSOR", 11); de[11] = 0x08; de += 32;
    memcpy(de, "\xE5" "ATA_9  TXT", 11); de[11] = 0x20; de[26] = 2; de[28] = 100; de += 32;
    for (uint32_t i = 0; i < nfiles; i++, de += 32)
    {
        memcpy(de, files[i].name83, 11);
        de[11] = 0x20;
        de[26] = (uint8_t)first[i]; de[27] = (uint8_t)(first[i] >> 8);
        de[28] = (uint8_t)files[i].size; de[29] = (uint8_t)(files[i].size >> 8);
        de[30] = (uint8_t)(files[i].size >> 16); de[31] = (uint8_t)(files[i].size >> 24);
    }
    return end;
}

/* Собрать из журнала тексты импорта по файлам и проверить байт в байт. */
static int import_check(const LogRing *r, const OldFileSpec *files, const uint32_t *order,
                        uint32_t nexp, uint8_t texts[][40000], int verbose)
{
    static uint8_t got[40000];
    LogRecord rec;
    uint32_t n = LogRing_Count(r), cur = 0xFFFFFFFFUL, len = 0, nfile = 0, end_seen = 0;
    int errors = 0;
    for (uint32_t i = 0; i <= n; i++)
    {
        uint8_t st = (i < n) ? LogRing_Read(r, i, &rec) : LOG_READ_NONE;
        if (st == LOG_READ_BAD) continue;
        uint8_t t = (st == LOG_READ_OK) ? LogRecord_Type(&rec) : 0;
        if (st != LOG_READ_OK || t == LOG_TYPE_IMPORT_FILE || t == LOG_TYPE_IMPORT_END)
        {
            if (cur != 0xFFFFFFFFUL)
            {
                const OldFileSpec *f = &files[order[nfile]];
                if (len != f->size || memcmp(got, texts[order[nfile]], len) != 0)
                {
                    if (verbose) printf("    файл %u (%s): %u байт из %u\n", nfile, f->name83, len, f->size);
                    errors++;
                }
                nfile++;
            }
            cur = (t == LOG_TYPE_IMPORT_FILE) ? rec.b[LOG_PAYLOAD_OFS] : 0xFFFFFFFFUL;
            len = 0;
            if (t == LOG_TYPE_IMPORT_END) { end_seen++; break; }
            if (st != LOG_READ_OK) break;
            continue;
        }
        const uint8_t *txt; uint8_t fidx;
        uint32_t l = LogRecord_Text(&rec, &txt, &fidx);
        if (!l || fidx != cur || len + l > sizeof(got)) { errors++; continue; }
        memcpy(got + len, txt, l);
        len += l;
    }
    if (nfile != nexp || end_seen != 1U) errors++;
    return errors;
}

static uint8_t old_texts[4][40000];

static void test_log_import(void)
{
    LogRing r;
    LogOldVolume v;
    puts("Импорт старого архива");

    /* data.txt — текущий, data_2 и data_1 — прежние форматы; в каталоге
     * лежат не по порядку. Порядок переноса: data_1, data_2, data.txt. */
    static const OldFileSpec F[3] = {
        { "DATA    TXT", 23456, 1 },
        { "DATA_2  TXT", 5000, 2 },
        { "DATA_1  TXT", 1100, 3 },
    };
    static const uint32_t ORDER[3] = { 2, 1, 0 };

    for (uint32_t bits = 12; bits <= 16; bits += 4)
    {
        for (int inter = 0; inter < 2; inter++)
        {
            char what[200];
            fm_reset(LOG_FLASH_SECTORS);
            uint32_t end = old_volume(bits, F, 3, inter, old_texts);
            static uint8_t before[64 * 4096];
            memcpy(before, fm, sizeof(before));

            equ(LogImport_Probe(&FM_OPS, LOG_FLASH_SECTORS * 4096U, &v), 1, "старый том узнан");
            ok(v.fatbits == bits && v.root_lba == 63U + 1U + MKFS_BOOT12[22] * (bits == 16U ? 2U : 1U) &&
               v.data_lba == v.root_lba + 32U && v.spc == (bits == 16U ? 1U : 2U),
               bits == 16U ? "загрузочный f_mkfs FAT16 разобран: корень 96, данные 128"
                           : "загрузочный f_mkfs FAT12 (кластер 1 КБ) разобран: корень 80, данные 112");
            snprintf(what, sizeof(what), "FAT%u%s: три файла по порядку, граница за данными",
                     bits, inter ? " вперемешку" : "");
            ok(v.nfiles == 3 && strcmp(v.files[0].name, "data_1.txt") == 0 &&
               strcmp(v.files[1].name, "data_2.txt") == 0 && strcmp(v.files[2].name, "data.txt") == 0 &&
               v.end >= end && v.end < end + 4096U && v.end % 4096U == 0U, what);

            equ(LogRing_Mount(&r, &FM_OPS, LOG_FLASH_SECTORS), LOG_MOUNT_FOREIGN, "том — чужое для журнала");
            equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 0, &v), LOG_OPEN_NEED_POWER,
                "без питания импорт не начинается");
            equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v), LOG_OPEN_OK, "импорт прошёл");
            snprintf(what, sizeof(what), "FAT%u%s: тексты в журнале совпали байт в байт", bits, inter ? " вперемешку" : "");
            equ((unsigned)import_check(&r, F, ORDER, 3, old_texts, 1), 0, what);
            ok(memcmp(before, fm, v.end) == 0, "старый том не тронут");

            /* Перезапуск: импорт закончен — повторно не идёт. */
            uint32_t cnt = LogRing_Count(&r);
            equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v), LOG_OPEN_OK, "перезапуск");
            equ(LogRing_Count(&r), cnt, "после перезапуска импорт не повторяется");
        }
    }

    /* Обрыв посреди импорта — после каждой операции записи. Повторный
     * старт доводит импорт, тексты совпадают, старый том цел. */
    {
        uint32_t points = 0, failed = 0;
        static const OldFileSpec G[2] = { { "DATA    TXT", 6000, 7 }, { "DATA_1  TXT", 700, 8 } };
        static const uint32_t GO[2] = { 1, 0 };
        for (long cut = 0;; cut++)
        {
            fm_reset(LOG_FLASH_SECTORS);
            fm_mode = (int)(cut % 3);
            fm_rng = 777U + (uint32_t)cut;
            uint32_t end = old_volume(16, G, 2, 1, old_texts);
            static uint8_t before[64 * 4096];
            memcpy(before, fm, sizeof(before));
            fm_ops = 0;
            fm_cut = cut;
            uint8_t res = LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
            if (!fm_dead)
            {
                if (res != LOG_OPEN_OK) failed++;
                break;
            }
            points++;
            /* Иногда — второй обрыв уже во время повтора. */
            fm_power_on();
            if (cut % 5 == 4)
            {
                fm_cut = fm_ops + (long)(fm_rand() % 60U);
                (void)LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
                fm_power_on();
            }
            int e = 0;
            if (LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v) != LOG_OPEN_OK) e++;
            if (memcmp(before, fm, (end + 4095U) / 4096U * 4096U) != 0) e++;
            if (import_check(&r, G, GO, 2, old_texts, failed < 3U)) e++;
            if (e && failed < 3U) printf("    обрыв на операции %ld\n", cut);
            failed += (e != 0);
        }
        char what[200];
        snprintf(what, sizeof(what), "обрыв посреди импорта (%u точек): архив цел, импорт доведён", points);
        ok(points > 100U && failed == 0U, what);
    }

    /* После импорта журнал идёт по кругу и заходит на старый том: тот
     * стирается, импорт не перезапускается, журнал читается. */
    {
        static const OldFileSpec H[1] = { { "DATA    TXT", 3000, 9 } };
        fm_reset(32);
        uint32_t end = old_volume(16, H, 1, 0, old_texts);
        (void)end;
        equ(LogArchive_Open(&r, &FM_OPS, 32, 1, &v), LOG_OPEN_OK, "малая флеш: импорт");
        LogRecord rec;
        uint32_t bad_open = 0;
        for (uint32_t id = 1; id <= 32U * 64U * 3U; id++)
        {
            log_make(&rec, id);
            (void)LogRing_Append(&r, &rec);
            if (id % 200U == 0U)
            {
                uint32_t cnt = LogRing_Count(&r);
                if (LogArchive_Open(&r, &FM_OPS, 32, 1, &v) != LOG_OPEN_OK || LogRing_Count(&r) != cnt)
                    bad_open++;
            }
        }
        equ(bad_open, 0U, "три круга после импорта: перезапуски не трогают журнал");
        uint32_t bad; int alien;
        uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
        ok(good + bad == LogRing_Count(&r) && bad == log_scan_skipped && !alien && good >= 29U * 64U,
           "журнал цел (испорченных нет, только пропуски после стартов), импорт вытеснен кругом");
    }

    /* Не помещается выше тома — переносятся самые новые файлы. */
    {
        static const OldFileSpec B[2] = { { "DATA    TXT", 8000, 4 }, { "DATA_1  TXT", 8000, 5 } };
        static const uint32_t BO[1] = { 0 };
        /* Флеш 100 КБ: том кончается на 80 КБ, выше — 5 секторов. Один файл
         * (156 записей, сектор перехода и запасной) в них влезает, два — нет. */
        fm_reset(25);
        (void)old_volume(16, B, 2, 0, old_texts);
        equ(LogArchive_Open(&r, &FM_OPS, 25, 1, &v), LOG_OPEN_OK, "не помещается — импорт без старейших");
        equ((unsigned)import_check(&r, B, BO, 1, old_texts, 1), 0, "перенесён самый новый файл, data.txt");
        LogRecord last;
        ok(LogRing_Newest(&r, &last, NULL) == LOG_READ_OK && LogRecord_Type(&last) == LOG_TYPE_IMPORT_END &&
           last.b[LOG_PAYLOAD_OFS + 5] == 1, "в IMPORT_END — один файл не поместился");
    }

    /* Не влезает и один файл — переносятся заголовок и самый новый хвост
     * с границы строки; срез отмечен в IMPORT_FILE. */
    {
        static const OldFileSpec T[1] = { { "DATA    TXT", 30000, 6 } };
        fm_reset(32);
        (void)old_volume(16, T, 1, 0, old_texts);
        equ(LogArchive_Open(&r, &FM_OPS, 32, 1, &v), LOG_OPEN_OK, "большой data.txt: импорт хвоста");
        static uint8_t got[40000];
        uint32_t len = 0, skip = 0, nf = 0, ends = 0;
        LogRecord rec;
        for (uint32_t i = 0; i < LogRing_Count(&r); i++)
        {
            if (LogRing_Read(&r, i, &rec) != LOG_READ_OK) continue;
            const uint8_t *t;
            uint32_t l = LogRecord_Text(&rec, &t, NULL);
            if (l) { memcpy(got + len, t, l); len += l; }
            else if (LogRecord_Type(&rec) == LOG_TYPE_IMPORT_FILE) { nf++; skip = LogRecord_Get32(rec.b + LOG_PAYLOAD_OFS + 18); }
            else if (LogRecord_Type(&rec) == LOG_TYPE_IMPORT_END) ends++;
        }
        const uint8_t *src = old_texts[0];
        uint32_t hdr = (uint32_t)((const uint8_t *)memchr(src, '\n', 30000) - src) + 1U;
        uint32_t tail = (len > hdr) ? len - hdr : 0U;
        ok(nf == 1 && ends == 1 && skip > 0 && skip < 30000, "большой data.txt: срез отмечен");
        ok(len > hdr && memcmp(got, src, hdr) == 0, "большой data.txt: заголовок сохранён");
        ok(memcmp(got + hdr, src + 30000 - tail, tail) == 0 && src[30000 - tail - 1] == '\n',
           "большой data.txt: хвост байт в байт с начала строки до конца");
        /* Выше тома (~94 КБ) — 8 секторов, из них 2 — переход и запасной:
         * текста влезает ~6 × 64 × 52 ≈ 19.9 КБ. */
        ok(tail > 15000U, "большой data.txt: перенесено почти всё, что влезло");
    }

    /* Не влезает ничего — журнал с нуля, но с отметкой о выпавшем архиве. */
    {
        static const OldFileSpec T[1] = { { "DATA    TXT", 3000, 6 } };
        fm_reset(18);   /* том кончается на 68 КБ, выше — 1 сектор: не хватает ни на что */
        (void)old_volume(16, T, 1, 0, old_texts);
        equ(LogArchive_Open(&r, &FM_OPS, 18, 1, &v), LOG_OPEN_OK, "не влезло ничего: журнал открыт");
        LogRecord last;
        ok(LogRing_Newest(&r, &last, NULL) == LOG_READ_OK && LogRecord_Type(&last) == LOG_TYPE_IMPORT_END &&
           last.b[LOG_PAYLOAD_OFS + 5] == 1, "не влезло ничего: в журнале IMPORT_END с одним выпавшим файлом");
    }

    /* Сбой чтения при проверке незаконченного импорта — не «импорта нет»:
     * открытие сообщает EIO, следующее доводит импорт. */
    {
        static const OldFileSpec G[1] = { { "DATA    TXT", 6000, 7 } };
        static const uint32_t GO[1] = { 0 };
        fm_reset(LOG_FLASH_SECTORS);
        fm_mode = 1;
        (void)old_volume(16, G, 1, 0, old_texts);
        fm_ops = 0;
        fm_cut = 40;
        (void)LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
        fm_power_on();
        /* Монтирование читает по 64 Б: 512 слотов 0 + хвост головы + запасной;
         * сбой — на первом чтении после него (это LogRing_Newest). */
        LogRing probe;
        uint32_t reads = 0;
        {
            /* Сосчитать чтения монтирования. */
            static long cnt;
            cnt = 0;
            fm_read_fail = 1L << 30;
            (void)LogRing_Mount(&probe, &FM_OPS, LOG_FLASH_SECTORS);
            cnt = (1L << 30) - fm_read_fail;
            reads = (uint32_t)cnt;
        }
        fm_read_fail = (long)reads;
        equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v), LOG_OPEN_EIO,
            "сбой чтения при проверке импорта — EIO, а не «журнал открыт»");
        fm_read_fail = -1;
        equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v), LOG_OPEN_OK, "следующее открытие доводит импорт");
        equ((unsigned)import_check(&r, G, GO, 1, old_texts, 1), 0, "после сбоя чтения импорт полон");
    }

    /* T1: отказ одного чтения при разборе старого тома — не «тома нет».
     * Сценарии стенда приёмки: загрузочный сектор (LBA 63), корень (LBA 96),
     * запись FAT первого кластера data.txt; на первом старте открытие
     * отвечает EIO и ничего не пишет, следующее переносит все три файла. */
    {
        static const OldFileSpec P[3] = {
            { "DATA_1  TXT", 1100, 3 }, { "DATA_2  TXT", 5000, 2 }, { "DATA    TXT", 23456, 1 } };
        static const uint32_t PO[3] = { 0, 1, 2 };
        static const char *what3[3] = { "загрузочный сектор", "корень", "FAT data.txt" };
        for (int which = 0; which < 3; which++)
        {
            char what[200];
            fm_reset(LOG_FLASH_SECTORS);
            fm_mode = 1;
            uint32_t end = old_volume(16, P, 3, 0, old_texts);
            static uint8_t before[64 * 4096];
            memcpy(before, fm, sizeof(before));
            fm_fail_len = 512;
            if (which == 0) fm_fail_addr = 63 * 512;
            if (which == 1) fm_fail_addr = 96 * 512;
            if (which == 2)
            {
                uint32_t c = fm[96 * 512 + 2 * 32 + 26] | (fm[96 * 512 + 2 * 32 + 27] << 8);
                fm_fail_addr = (long)(64U * 512U + c * 2U);
                fm_fail_len = 2;
            }
            long ops0 = fm_ops;
            uint8_t res = LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
            snprintf(what, sizeof(what), "T1 (%s): сбой чтения — EIO, ничего не записано", what3[which]);
            ok(res == LOG_OPEN_EIO && fm_fail_addr < 0 && fm_ops == ops0 && memcmp(before, fm, end) == 0, what);
            res = LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
            snprintf(what, sizeof(what), "T1 (%s): повтор переносит все три файла, том цел", what3[which]);
            ok(res == LOG_OPEN_OK && import_check(&r, P, PO, 3, old_texts, 1) == 0 &&
               memcmp(before, fm, end) == 0, what);
        }

        /* Незаконченный импорт и отказ чтения корня при повторе: не
         * «тома нет» — импорт не закрывается тем, что успели. */
        fm_reset(LOG_FLASH_SECTORS);
        fm_mode = 1;
        uint32_t end = old_volume(16, P, 3, 0, old_texts);
        static uint8_t before[64 * 4096];
        memcpy(before, fm, sizeof(before));
        fm_ops = 0;
        fm_cut = 200;
        (void)LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
        fm_power_on();
        fm_fail_addr = 96 * 512;
        fm_fail_len = 512;
        long ops0 = fm_ops;
        ok(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v) == LOG_OPEN_EIO && fm_ops == ops0,
           "T1: незаконченный импорт, сбой чтения корня — EIO, ничего не стёрто");
        ok(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v) == LOG_OPEN_OK &&
           import_check(&r, P, PO, 3, old_texts, 1) == 0 && memcmp(before, fm, end) == 0,
           "T1: следующий старт доводит импорт целиком");
    }

    /* T1: отказывает k-е чтение, для каждого k — на первом старте и при
     * незаконченном импорте. Открытие повторяется, как в main (до трёх раз,
     * потом в следующем цикле). Критерии: том ниже границы не тронут,
     * все файлы перенесены, а при ответе EIO на флеш не записано ничего.
     * При незаконченном импорте чтений ~35 тысяч (проверка чистоты
     * журнальной области): подряд — первые 1200 (монтирование, проверка
     * импорта, оба разбора тома — около 650), дальше каждое 499-е;
     * LOG_FULL=1 — все. */
    {
        static const OldFileSpec G[2] = { { "DATA    TXT", 6000, 7 }, { "DATA_1  TXT", 700, 8 } };
        static const uint32_t GO[2] = { 1, 0 };
        const int full = getenv("LOG_FULL") != NULL;
        for (int pend = 0; pend < 2; pend++)
        {
            uint32_t points = 0, failed = 0;
            for (long k = 0;; k = (!pend || full || k < 1200) ? k + 1 : k + 499)
            {
                fm_reset(LOG_FLASH_SECTORS);
                fm_mode = 1;
                uint32_t end = old_volume(16, G, 2, 1, old_texts);
                static uint8_t before[64 * 4096];
                memcpy(before, fm, sizeof(before));
                if (pend)
                {
                    fm_ops = 0;
                    fm_cut = 60;
                    (void)LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
                    fm_power_on();
                }
                long ops0 = fm_ops;
                fm_read_fail = k;
                uint8_t res = LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
                if (fm_read_fail >= 0)
                {
                    fm_read_fail = -1;
                    break;            /* чтений меньше k — точки кончились */
                }
                points++;
                int e = 0;
                if (res == LOG_OPEN_EIO && fm_ops != ops0) e++;
                for (int t = 0; t < 4 && res != LOG_OPEN_OK; t++)
                    res = LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v);
                if (res != LOG_OPEN_OK) e++;
                if (memcmp(before, fm, end) != 0) e++;
                if (import_check(&r, G, GO, 2, old_texts, 0)) e++;
                if (e && failed < 3U) printf("    %s, отказ чтения %ld: ошибок %d\n",
                                             pend ? "повтор импорта" : "первый старт", k, e);
                failed += (e != 0);
            }
            char what[200];
            snprintf(what, sizeof(what), "T1: отказ k-го чтения (%s, %u точек) — том цел, импорт полон",
                     pend ? "незаконченный импорт" : "первый старт", points);
            ok(points > (pend ? 1200U : 1000U) && failed == 0U, what);
        }
    }

    /* T1: то же для обрезанного большого data.txt (заголовок и хвост с
     * границы строки ищутся чтением): итог после сбоя — тот же, что без него. */
    {
        static const OldFileSpec T[1] = { { "DATA    TXT", 30000, 6 } };
        static LogRecord ref[32 * 64];
        fm_reset(32);
        fm_mode = 1;
        (void)old_volume(16, T, 1, 0, old_texts);
        (void)LogArchive_Open(&r, &FM_OPS, 32, 1, &v);
        uint32_t nref = LogRing_Count(&r);
        for (uint32_t i = 0; i < nref; i++) (void)LogRing_Read(&r, i, &ref[i]);
        uint32_t points = 0, failed = 0;
        for (long k = 0;; k++)
        {
            fm_reset(32);
            uint32_t end = old_volume(16, T, 1, 0, old_texts);
            static uint8_t before[32 * 4096];
            memcpy(before, fm, sizeof(before));
            fm_read_fail = k;
            uint8_t res = LogArchive_Open(&r, &FM_OPS, 32, 1, &v);
            if (fm_read_fail >= 0) { fm_read_fail = -1; break; }
            points++;
            for (int t = 0; t < 4 && res != LOG_OPEN_OK; t++)
                res = LogArchive_Open(&r, &FM_OPS, 32, 1, &v);
            int e = (res != LOG_OPEN_OK || memcmp(before, fm, end) != 0 || LogRing_Count(&r) != nref);
            LogRecord got;
            for (uint32_t i = 0; !e && i < nref; i++)
                e = (LogRing_Read(&r, i, &got) != LOG_READ_OK ||
                     memcmp(got.b, ref[i].b, 2) != 0 ||
                     memcmp(got.b + LOG_PAYLOAD_OFS, ref[i].b + LOG_PAYLOAD_OFS, LOG_PAYLOAD_SIZE) != 0);
            if (e && failed < 3U) printf("    большой data.txt, отказ чтения %ld\n", k);
            failed += (uint32_t)e;
        }
        char what[200];
        snprintf(what, sizeof(what), "T1: отказ k-го чтения (обрезанный data.txt, %u точек) — импорт как без сбоя", points);
        ok(nref > 300U && points > 500U && failed == 0U, what);
    }

    /* T5: место проверяется и перед записью FILE (и END): запись, стоящая
     * у края старого тома, импорт не начинает. */
    {
        static const OldFileSpec H[1] = { { "DATA    TXT", 3000, 9 } };
        fm_reset(32);
        fm_mode = 1;
        uint32_t end = old_volume(16, H, 1, 0, old_texts);
        static uint8_t before[64 * 4096];
        memcpy(before, fm, sizeof(before));
        equ(LogImport_Probe(&FM_OPS, 32U * 4096U, &v), LOG_PROBE_VOL, "T5: том узнан");
        uint32_t first = v.end / LOG_SECTOR_SIZE;
        LogRing_Start(&r, &FM_OPS, 32, first, 0);
        r.w_sect = 30;
        r.w_slot = LOG_SLOTS - 2U;   /* за ней — сектор 31 и запасной 0 (MBR) */
        r.w_erased = 1;
        r.spare_erased = 1;
        ok(LogImport_Run(&r, &v, 32U * 4096U, first) == 0 && memcmp(before, fm, end) == 0 &&
           r.next_seq == 0U, "T5: у края тома запись FILE не делается, MBR не стёрт");
    }

    /* Не наш том или флеш с мусором — журнал с начала, без импорта. */
    {
        fm_reset(16);
        memset(fm, 0x5A, 16U * 4096U);
        equ(LogImport_Probe(&FM_OPS, 16U * 4096U, &v), LOG_PROBE_NONE, "мусор — не том");
        equ(LogArchive_Open(&r, &FM_OPS, 16, 1, &v), LOG_OPEN_OK, "мусор: журнал открыт");
        LogRecord rec;
        uint32_t fails = 0;
        for (uint32_t id = 1; id <= 16U * 64U * 2U; id++)
        {
            log_make(&rec, id);
            fails += !LogRing_Append(&r, &rec);
        }
        uint32_t bad; int alien;
        uint32_t good = log_scan(&r, &bad, NULL, 0, &alien);
        ok(!fails && good == LogRing_Count(&r) && !bad && !alien, "мусор: журнал пишется поверх и читается");
    }

    /* Пустой том (файлов нет) — импортировать нечего. */
    {
        fm_reset(LOG_FLASH_SECTORS);
        (void)old_volume(16, F, 0, 0, old_texts);
        equ(LogArchive_Open(&r, &FM_OPS, LOG_FLASH_SECTORS, 1, &v), LOG_OPEN_OK, "пустой том: открыт");
        equ(LogRing_Count(&r), 0U, "пустой том: журнал пуст, импорта нет");
    }

    /* Для сверки независимым читателем FAT (pyfatfs): LOG_DUMP_DIR=<папка>
     * выкладывает образ тома и ожидаемые файлы. */
    const char *dump = getenv("LOG_DUMP_DIR");
    if (dump)
    {
        char path[512];
        for (uint32_t bits = 12; bits <= 16; bits += 4)
        {
            fm_reset(LOG_FLASH_SECTORS);
            (void)old_volume(bits, F, 3, 1, old_texts);
            snprintf(path, sizeof(path), "%s/fat%u.img", dump, bits);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(fm + 63U * 512U, 1, LOG_FLASH_SECTORS * 4096U - 63U * 512U, f); fclose(f); }
        }
        for (uint32_t i = 0; i < 3; i++)
        {
            snprintf(path, sizeof(path), "%s/%.11s", dump, F[i].name83);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(old_texts[i], 1, F[i].size, f); fclose(f); }
        }
    }
}

int main(void)
{
    test_ds18b20();
    test_battery();
    test_soil();
    test_aht20();
    test_bmp280();
    test_sleep_policy();
    test_usb_policy();
    test_watchdog_policy();
    test_quarantine_policy();
    test_storage_policy();
    test_time_trust();
    test_log_record();
    test_log_ring_g1();
    test_log_ring_g4();
    test_log_ring_g2();
    test_log_ring_accept2();
    test_log_import();

    printf("\n%d проверок, %d провалов\n", checks, failures);
    return failures ? 1 : 0;
}
