@echo off
rem Portable tests; no device, network or firmware flashing. Requires VS 2022 Build Tools.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "R=%~dp0.."
set "O=%R%\build\host-tests"
if not exist "%O%" mkdir "%O%"
pushd "%O%"
cl /nologo /std:clatest /O1 /W0 /utf-8 /D_CRT_SECURE_NO_WARNINGS /FI"%R%\test\msvc\compat.h" /I"%R%\test\msvc" ^
  /I"%R%\components\nt4\include" /I"%R%\components\bezel\include" /I"%R%\components\catalyst\include" /I"%R%\components\assist\include" /I"%R%\components\assist\src" /I"%R%\components\home\include" /I"%R%\components\intel\include" /I"%R%\components\intel\src" /I"%R%\components\tab_hal\include" /I"%R%\components\ui\src" ^
  "%R%\test\*.c" ^
  "%R%\components\nt4\src\mpack_lite.c" ^
  "%R%\components\nt4\src\json_lite.c" ^
  "%R%\components\bezel\src\bz_motion.c" ^
  "%R%\components\catalyst\src\cat_can.c" ^
  "%R%\components\catalyst\src\cat_logs.c" ^
  "%R%\components\catalyst\src\cat_batt.c" ^
  "%R%\components\catalyst\src\cat_sc.c" ^
  "%R%\components\assist\src\as_json.c" ^
  "%R%\components\assist\src\as_sse.c" ^
  "%R%\components\assist\src\as_conv.c" ^
  "%R%\components\assist\src\as_tooldefs.c" ^
  "%R%\components\assist\src\as_oai.c" ^
  "%R%\components\assist\src\as_desk.c" ^
  "%R%\components\assist\src\as_mem.c" ^
  "%R%\components\assist\src\as_diag.c" ^
  "%R%\components\assist\src\as_prompts.c" ^
  "%R%\components\home\src\home_capture_policy.c" ^
  "%R%\components\home\src\home_presence_policy.c" ^
  "%R%\components\home\src\home_art.c" ^
  "%R%\components\home\src\home_px.c" ^
  "%R%\components\home\src\home_tba_paths.c" ^
  "%R%\components\home\src\home_pet.c" ^
  "%R%\components\home\src\home_astro.c" ^
  "%R%\components\ui\src\ui_avatar_def.c" ^
  "%R%\components\tab_hal\src\wifi_saved.c" ^
  "%R%\components\intel\src\intel_nexus_parse.c" ^
  "%R%\components\intel\src\intel_rn_parse.c" ^
  "%R%\components\intel\src\intel_sb_parse.c" ^
  "%R%\components\intel\src\intel_rules.c" ^
  "%R%\components\intel\src\intel_store.c" ^
  "%R%\components\intel\src\intel_intent.c" ^
  "%R%\components\intel\src\intel_brief.c" ^
  "%R%\components\intel\src\intel_util.c" ^
  /Fe:catalyst_tab_tests.exe > build.log 2>&1
if errorlevel 1 (
  type build.log
  popd
  exit /b 1
)
catalyst_tab_tests.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
