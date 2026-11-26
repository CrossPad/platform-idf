@echo off
setlocal
rem keepawake.bat -- wake a CrossPad that has only dimmed, or keep one awake
rem through a build, a flash or a long analysis. Thin wrapper over
rem tools/keepawake.py, which says what the board's states mean and what a host
rem cannot do about soft-off (docs/subsystems/power-button.md).
rem
rem   tools\keepawake.bat status              what the board is doing
rem   tools\keepawake.bat wake                take it out of the dim state now
rem   tools\keepawake.bat keep  30            one 30-minute keep-awake lease
rem   tools\keepawake.bat watch 30            hold the lease, refreshing and printing
rem
rem Set CROSSPAD_HIL to the crosspad-hil entry point if it is not in one of the
rem places tools/bench.py looks in. Exit code 1 means the board is past reach:
rem it slept into soft-off, and the STM wakes on its power button, not on a
rem register -- press it once and run this again.

where python >nul 2>nul
if not errorlevel 1 (
    python "%~dp0keepawake.py" %*
    exit /b %errorlevel%
)
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0keepawake.py" %*
    exit /b %errorlevel%
)
echo no python on PATH: use the ESP-IDF shell, or the crosspad-hil venv
exit /b 2
