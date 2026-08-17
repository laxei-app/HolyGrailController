@echo off
rem Plan-owned capture control method regression test (no camera needed).
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat in the system codepage,
rem       so UTF-8 Japanese comments here would corrupt the script.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64.bat not found & exit /b 1 )

set HERE=%~dp0
set SRC=%HERE%..\..\10_common\src
rem NOTE: do NOT name this LIB - that is the linker library search path and
rem       overwriting it breaks the link ("cannot open libcpmt.lib").
set LIBDIR=%HERE%..\..\10_common\lib
set OUT=%HERE%build

if not exist "%OUT%" mkdir "%OUT%"

rem Astronomy Engine must not be built with fast-math (see memory: astronomy-engine-vendored).
cl /nologo /EHsc /std:c++17 /W3 /utf-8 /fp:precise ^
   /I "%SRC%" /I "%LIBDIR%" ^
   /Fo"%OUT%\\" /Fe"%OUT%\planCcmTest.exe" ^
   "%HERE%planCcmTest.cpp" "%SRC%\csJson.cpp" "%SRC%\astroSched.cpp" "%SRC%\exposureMath.cpp" ^
   "%SRC%\secret.cpp" "%SRC%\httpAuth.cpp" "%SRC%\md5.cpp" ^
   "%LIBDIR%\astronomy\astronomy.c"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

echo.
"%OUT%\planCcmTest.exe"
exit /b %errorlevel%
