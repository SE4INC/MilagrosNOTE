@echo off
REM Compila en tu PC (requiere Visual Studio 2022 con "Desarrollo para el escritorio con C++" y CMake)
cmake -B build -G "Visual Studio 17 2022" -A x64 || goto :error
cmake --build build --config Release --parallel || goto :error
echo.
echo Compilado: build\MilagrosNote_artefacts\Release\VST3\MilagrosNote.vst3
pause
exit /b 0
:error
echo Hubo un error. Copia el mensaje de arriba y pasaselo a Claude.
pause
exit /b 1
