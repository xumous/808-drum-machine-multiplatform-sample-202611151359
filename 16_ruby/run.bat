@echo off
rem ===========================================================================
rem  XU-808 Ruby edition - one-click launcher (by Xu)
rem  Usage:
rem    run.bat                  open GUI (no console window)
rem    run.bat --selftest       run self-test, write selftest_ruby.wav
rem    run.bat --play           open GUI and auto-play
rem    run.bat --export-wav     headless export 8s WAV
rem    run.bat --stems out      headless export 16 track stems to out\
rem  The bundled runtime lives in tools\ruby; system ruby is a fallback.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "RUBYDIR=%~dp0tools\ruby"
set "RUBY=%RUBYDIR%\bin\ruby.exe"
set "RUBYW=%RUBYDIR%\bin\rubyw.exe"

rem Point the bundled Ruby at its own stdlib and gems.
set "RUBYLIB=%RUBYDIR%\lib\ruby\4.0.0;%RUBYDIR%\lib\ruby\4.0.0\x64-mingw32"
set "GEM_HOME=%RUBYDIR%\lib\ruby\gems\4.0.0"
set "GEM_PATH=%GEM_HOME%"

if not exist "%RUBY%" (
  echo [WARN] bundled ruby not found: %RUBY%
  where ruby >nul 2>nul
  if errorlevel 1 (
    echo [ERROR] no ruby.exe in PATH. Install Ruby 4.x or restore tools\ruby.
    pause
    exit /b 2
  )
  set "RUBY=ruby"
  set "RUBYW=rubyw"
)

rem No arguments: double-click GUI mode, hide the console.
if "%~1"=="" (
  start "" "%RUBYW%" "%~dp0main.rb"
  exit /b 0
)

rem Arguments: console mode so SELFTEST OK / export logs stay visible.
"%RUBY%" "%~dp0main.rb" %*
set "CODE=%ERRORLEVEL%"
if not "%CODE%"=="0" pause
exit /b %CODE%
