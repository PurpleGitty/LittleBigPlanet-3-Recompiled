@echo off
rem Builds the runtime with clang-cl from Visual Studio (x64).
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S runtime -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl || exit /b 1
cmake --build build %*
