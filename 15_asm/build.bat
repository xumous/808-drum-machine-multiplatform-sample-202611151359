@echo off
rem ==========================================================================
rem  build.bat —— XU-808 ASM 版构建脚本（NASM + MinGW ld）
rem  用法：build.bat
rem ==========================================================================
setlocal
set NASM=..\tools\nasm\nasm-2.16.03\nasm.exe
set LD=C:\Qt\Tools\mingw1310_64\bin\ld.exe
set LIBDIR=C:\Qt\Tools\mingw1310_64\x86_64-w64-mingw32\lib

if not exist "%NASM%" set NASM=nasm
if not exist "%LD%" set LD=ld

echo [1/3] assemble
"%NASM%" -f win64 -o mathx.obj  mathx.asm  || goto :err
"%NASM%" -f win64 -o dsp.obj    dsp.asm    || goto :err
"%NASM%" -f win64 -o engine.obj engine.asm || goto :err
"%NASM%" -f win64 -o ui.obj     ui.asm     || goto :err
"%NASM%" -f win64 -o main.obj   main.asm   || goto :err

echo [2/3] link
"%LD%" -m i386pep --subsystem windows -e WinMainCRTStartup ^
  -o xu808_asm.exe mathx.obj dsp.obj engine.obj ui.obj main.obj ^
  -L "%LIBDIR%" -lkernel32 -luser32 -lgdi32 -lwinmm || goto :err

echo [3/3] done: xu808_asm.exe
exit /b 0

:err
echo BUILD FAILED
exit /b 1
