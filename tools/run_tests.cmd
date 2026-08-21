@echo off
rem Сборка и запуск хостовых тестов компилятором MSVC.
rem Отдельный toolchain не нужен: Visual Studio уже стоит на машине, а тесты
rem собираются под неё, а не под ARM — их смысл в том, чтобы проверять логику
rem без платы. Вызывать удобнее через tools/test.sh.
setlocal
set "ROOT=%~dp0.."
set "VCVARS=E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo Не найден vcvars64.bat: "%VCVARS%"
    echo Поправьте путь в tools\run_tests.cmd под свою установку Visual Studio.
    exit /b 2
)

call "%VCVARS%" >nul || exit /b 2
if not exist "%ROOT%\build\tests" mkdir "%ROOT%\build\tests"

cl /nologo /std:c11 /TC /W4 /I"%ROOT%\Core\Inc" ^
   "%ROOT%\tests\test_main.c" ^
   "%ROOT%\Core\Src\battery_curve.c" ^
   "%ROOT%\Core\Src\ds18b20_decode.c" ^
   "%ROOT%\Core\Src\power_policy.c" ^
   "%ROOT%\Core\Src\soil_curve.c" ^
   /Fo"%ROOT%\build\tests\\" /Fe"%ROOT%\build\tests\tests.exe" || exit /b 1

"%ROOT%\build\tests\tests.exe"
