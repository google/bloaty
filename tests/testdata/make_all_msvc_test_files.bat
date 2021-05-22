
@if not defined _echo echo off

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

for /f "usebackq delims=" %%i in (`%VSWHERE% -version [15.0^,17.0^) -property installationPath`) do (
    if exist "%%i\Common7\Tools\vsdevcmd.bat" (
        for %%A in (x86 x64) do (
            SETLOCAL
            call "%%i\Common7\Tools\vsdevcmd.bat" -arch=%%A
            ECHO Building VS %VisualStudioVersion% %%A
            call make_msvc_test_files.bat PE\%%A
            ENDLOCAL
        )
    )
)

