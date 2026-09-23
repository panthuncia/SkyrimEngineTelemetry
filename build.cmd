@echo off
setlocal

pushd "%~dp0" || exit /b 1

set "MO2_MOD_DIR=C:\Modding\MO2\mods\Skyrim Engine Telemetry"

echo Configuring SkyrimEngineTelemetry...
cmake --preset vs2026-msvc -DSET_SKYRIM_DIR="%MO2_MOD_DIR%"
if errorlevel 1 goto :failed

echo Building and deploying RelWithDebInfo to:
echo   %MO2_MOD_DIR%
cmake --build --preset relwithdebinfo-msvc --target deploy
if errorlevel 1 goto :failed

echo.
echo Build and deployment completed successfully.
popd
exit /b 0

:failed
set "BUILD_EXIT_CODE=%errorlevel%"
echo.
echo Build or deployment failed with exit code %BUILD_EXIT_CODE%.
popd
exit /b %BUILD_EXIT_CODE%
