@echo off
rem ============================================================
rem  Windows: pull + configure + build + sign (all in one)
rem  usage: build.bat [--pull-only] [--no-pull]
rem  deps : git / cmake / ninja / python (sign also needs
rem         "pip install imgtool")
rem  toolchain: Zephyr SDK 0.17.0 (arm-zephyr-eabi) by default;
rem         override with env TOOLCHAIN_PATH / TRIPLET
rem  NOTE : keep this file ASCII-only (CMD parses it as GBK)
rem ============================================================
setlocal
cd /d "%~dp0"

if "%TOOLCHAIN_PATH%"=="" set "TOOLCHAIN_PATH=C:/Users/jxwaz/zephyr-sdk-0.17.0/arm-zephyr-eabi"
if "%TRIPLET%"=="" set "TRIPLET=arm-zephyr-eabi"

if "%~1"=="--no-pull" goto configure

echo === [1/3] submodules (selective, no --recursive) ===
git submodule update --init
if errorlevel 1 goto fail
rem nested submodules must be addressed from inside their parent
git -C deps/STM32CubeF4 submodule update --init Drivers/STM32F4xx_HAL_Driver Drivers/CMSIS/Device/ST/STM32F4xx
if errorlevel 1 goto fail
git -C deps/mcuboot submodule update --init ext/mbedtls
if errorlevel 1 goto fail

if "%~1"=="--pull-only" goto done

:configure
echo === [2/3] configure + build (TOOLCHAIN=%TOOLCHAIN_PATH% TRIPLET=%TRIPLET%) ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE=deps/stm32-cmake/cmake/stm32_gcc.cmake ^
  -DSTM32_TOOLCHAIN_PATH=%TOOLCHAIN_PATH% ^
  -DSTM32_TARGET_TRIPLET=%TRIPLET% ^
  -DSTM32_CUBE_F4_PATH=deps/STM32CubeF4 ^
  -DFREERTOS_PATH=deps/FreeRTOS-Kernel
if errorlevel 1 goto fail
cmake --build build
if errorlevel 1 goto fail

echo === [3/3] sign ===
if not exist tools\keys\root-rsa2048.pem (
  echo [skip] tools\keys\root-rsa2048.pem not found, signing skipped.
  echo        The private key is never committed; copy the tools\keys
  echo        directory from the original dev machine to enable signing.
  goto done
)
python tools\sign_fw.py
if errorlevel 1 goto fail
echo artifacts: build\boot.hex build\fw.hex build\full.hex ^(full-chip flash^)
goto done

:fail
echo *** FAILED ***
exit /b 1

:done
echo OK
