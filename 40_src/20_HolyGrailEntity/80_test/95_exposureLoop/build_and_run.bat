@echo off
rem Exposure control loop regression test (no camera needed).
rem NOTE: keep this file ASCII-only. cmd.exe reads .bat in the system codepage,
rem       so UTF-8 Japanese comments here would corrupt the script.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64.bat not found & exit /b 1 )

set HERE=%~dp0
set SRC=%HERE%..\..\10_common\src
set OUT=%HERE%build

if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /EHsc /std:c++17 /W3 /utf-8 ^
   /I "%SRC%" ^
   /Fo"%OUT%\\" /Fe"%OUT%\exposureLoopTest.exe" ^
   "%HERE%exposureLoopTest.cpp" "%SRC%\exposureMath.cpp"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

echo.
"%OUT%\exposureLoopTest.exe"
exit /b %errorlevel%
