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
#include "soil_curve.h"
#include "storage_policy.h"

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
    static unsigned char mbr[STORAGE_SECTOR_BYTES], boot[STORAGE_SECTOR_BYTES];
    puts("Хранилище");

    /* Чистая флеш (оба сектора — 0xFF) и FatFs не видит ФС: это новый прибор,
     * форматировать можно и нужно. */
    memset(mbr, 0xFF, sizeof(mbr));
    memset(boot, 0xFF, sizeof(boot));
    equ(Storage_MayFormat(1, 1, mbr, boot), 1, "чистая флеш форматируется");

    /* ФС смонтировалась — форматировать нечего, какое бы содержимое ни было. */
    equ(Storage_MayFormat(0, 1, mbr, boot), 0, "есть ФС — не форматируем");

    /* Чтение сорвалось: буфер мог остаться 0xFF от прошлого раза, верить ему нельзя. */
    equ(Storage_MayFormat(1, 0, mbr, boot), 0, "ошибка чтения — не форматируем");

    /* Один байт не 0xFF в последнем байте MBR — там что-то лежало. Это ровно
     * сценарий аудита: испорченная сигнатура 55AA давала FR_NO_FILESYSTEM. */
    mbr[STORAGE_SECTOR_BYTES - 1] = 0xAA;
    equ(Storage_MayFormat(1, 1, mbr, boot), 0, "непустой MBR — архив не стираем");
    mbr[STORAGE_SECTOR_BYTES - 1] = 0xFF;

    /* MBR стёрт (питание пропало посреди его перезаписи), но загрузочный
     * сектор раздела жив — архив на месте, стирать нельзя. */
    boot[0] = 0xEB;
    equ(Storage_MayFormat(1, 1, mbr, boot), 0, "стёртый MBR при живом загрузочном — не форматируем");
    boot[0] = 0xFF;

    /* Мусор с мёртвой шины без подтяжки — тоже «не чисто». */
    boot[STORAGE_SECTOR_BYTES / 2] = 0x00;
    equ(Storage_MayFormat(1, 1, mbr, boot), 0, "мусор в загрузочном секторе — не форматируем");
    boot[STORAGE_SECTOR_BYTES / 2] = 0xFF;

    /* R3: сорванный формат. Загрузочный сектор — как его кладёт f_mkfs для
     * тома 4 МБ с кластером 512 (резерв 1, одна FAT по 32 сектора, раздел
     * с LBA 63): корень в секторе 63 + 1 + 32 = 96. MBR ещё стёрт. */
    {
        static unsigned char root[STORAGE_SECTOR_BYTES];
        static const unsigned char jmp_oem[11] = {0xEB,0xFE,0x90,'M','S','D','O','S','5','.','0'};

        memset(mbr, 0xFF, sizeof(mbr));
        memset(boot, 0, sizeof(boot));
        memcpy(boot, jmp_oem, sizeof(jmp_oem));
        boot[11] = 0x00; boot[12] = 0x02;   /* 512 байт на сектор */
        boot[13] = 1;                       /* кластер 1 сектор */
        boot[14] = 1;    boot[15] = 0;      /* резерв 1 */
        boot[16] = 1;                       /* одна FAT */
        boot[22] = 32;   boot[23] = 0;      /* FAT 32 сектора */
        boot[28] = 63;                      /* раздел с LBA 63 */
        boot[510] = 0x55; boot[511] = 0xAA;
        equ(Storage_OurRootDirSector(boot), 96, "наш загрузочный сектор — корень в 96");
        boot[16] = 2;
        equ(Storage_OurRootDirSector(boot), 128, "две FAT — корень за обеими, в 128");
        boot[16] = 1;

        /* f_mkfs оборвался до очистки корня: корень ещё 0xFF. */
        memset(root, 0xFF, sizeof(root));
        equ(Storage_MayFormat(1, 1, mbr, boot), 0, "наш недоделанный формат — «чистым» не считается");
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 1, "недоделанный формат, корень 0xFF — доделываем");

        /* Корень очищен, но MBR не записан. */
        memset(root, 0x00, sizeof(root));
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 1, "недоделанный формат, корень обнулён — доделываем");

        /* Прерванная правка типа раздела на томе с архивом: MBR стёрт, в корне data.txt. */
        memcpy(root, "DATA    TXT", 11);
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 0, "в корне data.txt — архив не стираем");
        /* Удалённый файл — тоже запись, даже если от неё осталась одна метка 0xE5. */
        memset(root, 0x00, sizeof(root));
        root[0] = 0xE5;
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 0, "в корне удалённый файл — не стираем");
        memset(root, 0x00, sizeof(root));

        equ(Storage_MayRedoFormat(1, 1, 0, mbr, boot, root), 0, "корень не прочитан — не форматируем");
        equ(Storage_MayRedoFormat(1, 0, 1, mbr, boot, root), 0, "MBR/загрузочный не прочитаны — не форматируем");
        equ(Storage_MayRedoFormat(0, 1, 1, mbr, boot, root), 0, "ФС смонтировалась — не форматируем");

        /* MBR не стёрт (битая сигнатура при живом разделе) — не наш случай. */
        mbr[0] = 0x00;
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 0, "MBR не стёрт — не форматируем");
        mbr[0] = 0xFF;

        /* Чужой загрузочный сектор (другой OEM — форматировал не наш f_mkfs). */
        boot[3] = 'm';
        equ(Storage_OurRootDirSector(boot), 0, "чужой OEM — не наш формат");
        equ(Storage_MayRedoFormat(1, 1, 1, mbr, boot, root), 0, "чужой загрузочный сектор — не форматируем");
        boot[3] = 'M';
        boot[511] = 0x00;
        equ(Storage_OurRootDirSector(boot), 0, "без 55AA — не наш формат");
        boot[511] = 0xAA;
        boot[28] = 0;
        equ(Storage_OurRootDirSector(boot), 0, "раздел не с LBA 63 — не наш формат");
        boot[28] = 63;
        boot[16] = 0;
        equ(Storage_OurRootDirSector(boot), 0, "ноль FAT — не загрузочный сектор");
        boot[16] = 1;

        memset(mbr, 0xFF, sizeof(mbr));
        memset(boot, 0xFF, sizeof(boot));
    }

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
 * собран на хосте с RAM-диском на 8192 сектора, параметры как в main.c
 * (FM_FAT, кластер 512 Б) и как у старых прошивок (кластер 1 КБ, FAT12).
 * Первые 64 байта сектора; остальное до сигнатуры — нули. Записи разделов
 * взяты из того же прогона, а не посчитаны этим кодом. */
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

static void mkfs_sector(unsigned char *sec, const unsigned char *first64)
{
    memset(sec, 0, 512);
    memcpy(sec, first64, 64);
    sec[510] = 0x55; sec[511] = 0xAA;
}

static void test_mbr_restore(void)
{
    unsigned char boot[512], mbr[512], built[512], want[512];

    puts("Восстановление MBR");

    mkfs_sector(boot, MKFS_BOOT16);
    memset(mbr, 0xFF, sizeof(mbr));

    /* Прерванная правка байта типа на томе с архивом: MBR стёрт, наш
     * загрузочный сектор жив. Восстанавливать. */
    equ(Storage_MayRestoreMbr(1, 1, mbr, boot, 8192), 1, "MBR стёрт, загрузочный наш — восстанавливаем");

    /* Восстановленный MBR — байт в байт как от f_mkfs, кроме типа: 0x0E, как
     * его оставляет прошивка для FAT16 (Android). */
    Storage_BuildMbr(boot, built);
    memset(want, 0, sizeof(want));
    memcpy(want + 446, MKFS_PTE16, 16);
    want[446 + 4] = 0x0E;
    want[510] = 0x55; want[511] = 0xAA;
    ok(memcmp(built, want, 512) == 0, "FAT16: MBR совпал с f_mkfs (тип 0x0E)");

    /* Старый FAT12 (кластер 1 КБ) — тип 0x01, ровно как у f_mkfs: 0x0E для
     * него был бы ложью. */
    mkfs_sector(boot, MKFS_BOOT12);
    Storage_BuildMbr(boot, built);
    memset(want, 0, sizeof(want));
    memcpy(want + 446, MKFS_PTE12, 16);
    want[510] = 0x55; want[511] = 0xAA;
    ok(memcmp(built, want, 512) == 0, "FAT12: MBR совпал с f_mkfs (тип 0x01)");

    /* Граница FAT12/FAT16 — как у FatFs при монтировании: 4085 кластеров —
     * ещё FAT12, 4086 — уже FAT16. Служебных секторов у FAT16-образа 65. */
    mkfs_sector(boot, MKFS_BOOT16);
    boot[19] = (unsigned char)((65 + 4085) & 0xFF); boot[20] = (unsigned char)((65 + 4085) >> 8);
    Storage_BuildMbr(boot, built);
    equ(built[446 + 4], 0x01, "4085 кластеров — FAT12");
    boot[19] = (unsigned char)((65 + 4086) & 0xFF); boot[20] = (unsigned char)((65 + 4086) >> 8);
    Storage_BuildMbr(boot, built);
    equ(built[446 + 4], 0x0E, "4086 кластеров — FAT16");

    /* Чего не трогаем. */
    mkfs_sector(boot, MKFS_BOOT16);
    equ(Storage_MayRestoreMbr(0, 1, mbr, boot, 8192), 0, "том смонтировался — MBR не трогаем");
    equ(Storage_MayRestoreMbr(1, 0, mbr, boot, 8192), 0, "чтение сорвалось — не решаем");

    mbr[0] = 0x00;
    equ(Storage_MayRestoreMbr(1, 1, mbr, boot, 8192), 0, "в MBR что-то лежит — не наш случай");
    memset(mbr, 0xFF, sizeof(mbr));

    unsigned char alien[512];
    memcpy(alien, boot, 512);
    alien[3] = 'X';   /* чужой OEM */
    equ(Storage_MayRestoreMbr(1, 1, mbr, alien, 8192), 0, "загрузочный сектор чужой — не трогаем");

    /* Том не помещается на диск (BPB битый) — по догадке не пишем. */
    equ(Storage_MayRestoreMbr(1, 1, mbr, boot, 8191), 0, "том с LBA 63 не влезает в диск — нет");
    equ(Storage_MayRestoreMbr(1, 1, mbr, boot, 63 + 8129), 1, "том ровно до конца диска — да");
    memcpy(alien, boot, 512);
    alien[19] = 0; alien[20] = 0;   /* TotSec16 = 0 и TotSec32 = 0 */
    equ(Storage_MayRestoreMbr(1, 1, mbr, alien, 8192), 0, "размер тома ноль — нет");
    alien[32] = 0xC1; alien[33] = 0x1F;   /* размер в 32-битном поле */
    equ(Storage_BootTotalSectors(alien), 8129, "размер берётся и из 32-битного поля BPB");
}

int main(void)
{
    test_ds18b20();
    test_battery();
    test_soil();
    test_aht20();
    test_sleep_policy();
    test_usb_policy();
    test_watchdog_policy();
    test_quarantine_policy();
    test_storage_policy();
    test_mbr_restore();

    printf("\n%d проверок, %d провалов\n", checks, failures);
    return failures ? 1 : 0;
}
