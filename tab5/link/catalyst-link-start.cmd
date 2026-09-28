@echo off
rem Start Catalyst Link for the tablet, quietly, at log-on.
rem
rem A shortcut to this lives in the user's Startup folder, so the tablet has something to talk to without
rem anyone remembering to start it. Remove it from
rem   %%APPDATA%%\Microsoft\Windows\Start Menu\Programs\Startup
rem to stop that; this file is only the command, and running it by hand still works.
rem
rem pythonw rather than python: no console window at log-on. Output goes to the log below instead, which
rem is where the pairing code and the reason for any refusal end up.
rem
rem Not `python -m catalyst_link.cli`: cli.py has no __main__ guard, so -m imports it and exits 0 without
rem a word -- which looks exactly like the service starting and then dying. The entry point is main().

set REPO=%~1
if "%REPO%"=="" set REPO=C:\Users\yu_th\dev\_worktrees\5805-offseason2026-code-systemcore

set LINKDIR=%~dp0
set LOG=%LOCALAPPDATA%\catalyst-link\link.log
if not exist "%LOCALAPPDATA%\catalyst-link" mkdir "%LOCALAPPDATA%\catalyst-link"

cd /d "%LINKDIR%"
start "" /b pythonw -c "from catalyst_link.cli import main; import sys; sys.argv=['catalyst-link','serve','--repo',r'%REPO%','--bind','0.0.0.0']; sys.exit(main())" >> "%LOG%" 2>&1
