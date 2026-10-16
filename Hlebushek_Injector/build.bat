@echo off
rem Builds ONE self-contained Hlebushek_Injector.exe (the menu DLL is embedded inside).
rem The game is 32-bit, so build as x86: MinGW-w64 i686 (MSYS2 MINGW32) or VS "x86 Native Tools Command Prompt".
cd /d "%~dp0"
if not exist bin mkdir bin

where cl >nul 2>nul
if %errorlevel%==0 goto MSVC

set CXX=i686-w64-mingw32-g++
where %CXX% >nul 2>nul || set CXX=g++
where %CXX% >nul 2>nul || (echo Compiler not found. Install MinGW-w64 i686 or Visual Studio. & exit /b 1)

%CXX% -m32 -O2 -s -std=c++11 -shared -static -o bin\hlebushek_menu.dll src\hlebushek_menu.cpp -lcomctl32 -lgdi32 -luser32 -lshell32
if errorlevel 1 goto FAIL
%CXX% -m32 -O2 -s -o bin\bin2h.exe tools\bin2h.cpp
if errorlevel 1 goto FAIL
bin\bin2h.exe bin\hlebushek_menu.dll bin\dll_blob.h
if errorlevel 1 goto FAIL
%CXX% -m32 -O2 -s -std=c++11 -municode -static -DEMBED_DLL -Ibin -o bin\Hlebushek_Injector.exe src\hlebushek_injector.cpp
if errorlevel 1 goto FAIL
goto OK

:MSVC
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" echo WARNING: use "x86 Native Tools Command Prompt for VS", otherwise you get a 64-bit build.
cl /nologo /utf-8 /EHsc /O2 /MT /LD /DUNICODE /D_UNICODE src\hlebushek_menu.cpp /Fe:bin\hlebushek_menu.dll /Fo:bin\ user32.lib gdi32.lib comctl32.lib shell32.lib
if errorlevel 1 goto FAIL
cl /nologo /O2 /EHsc /Fe:bin\bin2h.exe /Fo:bin\ tools\bin2h.cpp
if errorlevel 1 goto FAIL
bin\bin2h.exe bin\hlebushek_menu.dll bin\dll_blob.h
if errorlevel 1 goto FAIL
cl /nologo /utf-8 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DEMBED_DLL /Ibin src\hlebushek_injector.cpp /Fe:bin\Hlebushek_Injector.exe /Fo:bin\ user32.lib
if errorlevel 1 goto FAIL

:OK
echo.
echo Done: bin\Hlebushek_Injector.exe  (single file, the DLL is embedded)
exit /b 0

:FAIL
echo Build failed.
exit /b 1
