@echo off

if "%1"=="" goto noargs

pushd %1

@REM /Gm- disables minimal rebuild, /O1 favor size, /MD selects external runtime,
@REM /GL enable cross-module optimization
set CL_COMMON_FLAGS=/nologo /Gm- /O1 /MD /GL


call :make_obj ..\..\foo.c
call :make_obj ..\..\bar.c
call :make_obj ..\..\main.c

call :make_dll msvc-%VisualStudioVersion%-foo-bar.dll foo.obj bar.obj
call :make_binary_with_pdb msvc-%VisualStudioVersion%-foo-bar-main-cv.bin msvc-%VisualStudioVersion%-foo-bar-main-cv.pdb foo.obj bar.obj main.obj

goto cleanup

:make_dll:
for /f "tokens=1,* delims= " %%a in ("%*") do set ALL_BUT_FIRST=%%b
cl %CL_COMMON_FLAGS% /LD %ALL_BUT_FIRST% /link /OUT:%1
exit /B 0

:make_binary_with_pdb:
for /f "tokens=2,* delims= " %%a in ("%*") do set ALL_BUT_FIRST=%%b
cl %CL_COMMON_FLAGS% %ALL_BUT_FIRST% /link /OUT:%1 /PDB:%2 /DEBUG
exit /B 0

:make_obj:
cl %CL_COMMON_FLAGS% /c %1
exit /B 0

:noargs:
echo Usage: make_test_files.bat ^<output dir^>

:cleanup:

del foo.obj
del bar.obj
del main.obj

popd
