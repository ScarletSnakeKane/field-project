#ifndef STORAGE_POLICY_H
#define STORAGE_POLICY_H

/* Решения о том, когда трогать архив на флеш. Без HAL и без регистров —
 * чтобы их можно было проверить хостовыми тестами. Отдельный .c не заведён
 * сознательно: Debug/ не в git, и сборка CubeIDE новый файл не увидит. */

#include <stdint.h>

/* Логический сектор FAT и место загрузочного сектора раздела. f_mkfs без
 * FM_SFD кладёт раздел с LBA 63 (ff.c, b_vol), MBR — в сектор 0. */
#define STORAGE_SECTOR_BYTES   512U
#define STORAGE_BOOT_SECTOR    63U

/* Ниже этого VDD стирание и программирование SST26VF016B идут вне паспорта
 * (2.7–3.6 В), а пик тока стирания на исходе банки может уронить питание
 * посреди RMW блока каталога или FAT. Запас 0.2 В над паспортом — на просадку
 * под нагрузкой, которую VREFINT в момент замера не видит. */
#define STORAGE_MIN_VDD_VOLTS  2.9f

/* Весь буфер — 0xFF, то есть стёртая флеш. */
static inline uint8_t Storage_IsErased(const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != 0xFF)
            return 0;
    return 1;
}

/* Можно ли форматировать том. Раньше хватало FR_NO_FILESYSTEM от f_mount —
 * то есть одного искажённого чтения сектора 0 или 63, чтобы f_mkfs стёр архив
 * за месяцы. Теперь форматируется только флеш, на которой заведомо нечего
 * терять: оба сектора прочитаны без ошибки и стёрты целиком. Любой байт не
 * 0xFF означает, что там что-то лежало, — пусть битое, но разбирать его
 * будет человек, а не автоформат.
 *   no_fs    — f_mount вернул FR_NO_FILESYSTEM;
 *   reads_ok — оба сектора прочитаны без ошибки драйвера. */
static inline uint8_t Storage_MayFormat(uint8_t no_fs, uint8_t reads_ok,
                                        const uint8_t *mbr, const uint8_t *boot)
{
    if (!no_fs || !reads_ok)
        return 0;
    return Storage_IsErased(mbr, STORAGE_SECTOR_BYTES) &&
           Storage_IsErased(boot, STORAGE_SECTOR_BYTES);
}

/* Можно ли MSC (из прерывания OTG) читать флеш прямо сейчас. Не готова —
 * значит не инициализирована или спит, и будить её из прерывания нельзя:
 * задержки там не идут. Занята — основной поток посреди операции FatFs, и
 * чтение оборвало бы его кадр SPI или отдало хосту полузаписанный том.
 * В обоих случаях хост получает «не готов» и повторяет запрос сам. */
static inline uint8_t Storage_MscMayAccess(uint8_t flash_ready, uint8_t flash_busy)
{
    return (flash_ready && !flash_busy) ? 1U : 0U;
}

/* Можно ли сейчас писать на флеш при таком питании. Каждая дозапись — это
 * стирание и перепрограммирование 4-КБ блоков каталога и FAT, и сбой посреди
 * них теряет весь data.txt. Пропущенная строка стоит одного часа данных.
 * Неудавшийся замер батареи (valid == 0, vdd == 0) запись не запрещает:
 * иначе отказ АЦП навсегда остановил бы архив, а о питании мы тогда просто
 * ничего не знаем. */
static inline uint8_t Storage_MayWrite(uint8_t bat_valid, uint8_t bat_low, float vdd)
{
    if (!bat_valid)
        return 1;
    if (bat_low)
        return 0;
    return (vdd >= STORAGE_MIN_VDD_VOLTS) ? 1U : 0U;
}

#endif
