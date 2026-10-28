@echo off
rem Usage: tools\cmake_build.bat <target...>   (configures build/cmake with Ninja + clang-cl, Release)
rem KIG_BUILD_DIR overrides the build directory (tools/verify_recompile.py uses build/cmake-verify).
rem KIG_CMAKE_ARGS adds configure arguments, e.g. the trace/coverage build:
rem   set KIG_BUILD_DIR=build/cmake-trace & set KIG_CMAKE_ARGS=-DKIG_TRACE=ON & tools\cmake_build.bat kig
if "%1"=="" (echo usage: cmake_build.bat target & exit /b 2)
if "%KIG_BUILD_DIR%"=="" set KIG_BUILD_DIR=build/cmake
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S . -B %KIG_BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl %KIG_CMAKE_ARGS% > build\cmake_configure.log 2>&1 || (type build\cmake_configure.log & exit /b 1)
cmake --build %KIG_BUILD_DIR% --target %* > build\cmake_build.log 2>&1
set RC=%ERRORLEVEL%
exit /b %RC%
