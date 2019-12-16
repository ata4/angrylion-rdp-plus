@echo off
:: Last modification: 2019/Dec/06
cls
pushd "%~dps0"
set ms=ERROR
set ty0=%CD%
set ty1=Release
set ty2=Win32
set ty3=x86
set ty4=143
set ty5=1
set mod=0
set wrp=sk1
:sk2
if /i "%1"=="-X86" set mod=-1
if /i "%1"=="-X64" (
if "%PROCESSOR_ARCHITECTURE%" NEQ "AMD64" goto err
set ty2=x64
set ty3=x64
set mod=1
)
set ty6=
set xp=
msbuild /version >nul 2>&1
if errorlevel 1 goto err
7z >nul 2>&1
if errorlevel 1 goto err
git --version >nul 2>&1
if errorlevel 1 goto err
python -V >nul 2>&1
if errorlevel 1 goto err
goto cmk
:pak
if not exist "%ty0%\..\src\core\version.h" goto err
for /f "tokens=3" %%a in ('type "%ty0%\..\src\core\version.h" ^| find "define GIT_TAG"') do set v1=%%~a
for /f "tokens=3" %%a in ('type "%ty0%\..\src\core\version.h" ^| find "GIT_COMMIT_HASH"') do set v2=%%~a
for /f "tokens=3" %%a in ('type "%ty0%\..\src\core\version.h" ^| find "GIT_COMMIT_DATE"') do set v4=%%~a
for /f "tokens=1,2,3 delims=-" %%a in ('echo %v4%') do set v4=%%a%%b%%c
7z a -y -t7z "%ty0%\angrylion-plus_%v1%-%v2%_%v3%_%v4%.7z" *.dll
if errorlevel 1 goto err
goto:eof
:cmk
cmake --version >nul 2>&1
if errorlevel 1 goto vs
pushd "%ProgramFiles%\MSBuild\Microsoft.Cpp\v4.0"
if %errorlevel%==0 goto sk0
pushd "%ProgramFiles(x86)%\MSBuild\Microsoft.Cpp\v4.0"
if errorlevel 1 goto err
:sk0
if %mod%==1 goto sk1
dir /b /s *.targets | find /i "%ty2%" | find /i "_xp"
if %errorlevel%==0 set xp=_xp
:sk1
set /a ty4=%ty4%-%ty5%
if %ty4% LEQ 100 goto err
if %ty4%==139 set ty4=120& set ty5=10
dir /b /s *.targets | find /i "Platforms\%ty2%\PlatformToolsets\v%ty4%%xp%"
if errorlevel 1 goto %wrp%
set ty6=v%ty4%%xp%
pushd "%ty0%"
md ..\build_%ty3%
cd ..\build_%ty3%
if errorlevel 1 goto err
cmake -T "%ty6%" -A "%ty2%" ..
if errorlevel 1 goto err
cmake --build . --config %ty1%
if errorlevel 1 goto err
set v3=%ty3%-shared_%ty6%
cd %ty1%
if errorlevel 1 goto err
call :pak
:vs
pushd "%ty0%"
cd ..\msvc
if errorlevel 1 goto err
msbuild angrylion-plus.sln /t:Rebuild /p:Configuration=%ty1%;Platform=%ty3%
if errorlevel 1 goto err
set v3=%ty3%
cd %ty2%\%ty1%
if errorlevel 1 goto err
call :pak
if "%PROCESSOR_ARCHITECTURE%"=="AMD64" set /a mod=%mod%+1
set ty5=0
set wrp=err
if %mod%==1 call :sk2 -X64
set ms=DONE
echo.
:err
echo %ms%!
pushd "%ty0%"
