@echo on
if "%~1" == "" goto usage
if "%~2" == "" goto usage

if "%~2" == "true" (
    set buildType=Release
) else (
    if "%~2" == "false" (
        set buildType=Debug
    ) else goto usage
)

if "%~1" == "x86" goto x86 else (
    if "%~1" == "x64" goto x64 else goto usage
)

:x86
call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" x86
set prefixPath=C:\Qt\5.9.5\msvc2015
set archSuffix="x86"
goto build

:x64
call "C:\Program Files\Microsoft SDKs\Windows\v7.1\Bin\SetEnv.cmd" /x64
call "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" x86_amd64
set prefixPath=C:\Qt\5.9.5\msvc2015_64
set archSuffix="x64"
goto build

:build
mkdir build
pushd build
cmake -DARCH_LIB_SUFFIX=%archSuffix% -DCMAKE_PREFIX_PATH=%prefixPath% -DCMAKE_BUILD_TYPE=%buildType% -G "Ninja" ..
ninja
goto exit

:usage
echo "Usage build.cmd [x86|x64] [true|false]"
goto exit

:exit
popd
