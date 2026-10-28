@echo off
chcp 65001 >nul
setlocal

rem 从脚本所在目录向上定位 Git 仓库根目录；在交付包内运行也能操作主仓库。
set "REPO_ROOT="
for /f "delims=" %%i in ('git -C "%~dp0" rev-parse --show-toplevel 2^>nul') do set "REPO_ROOT=%%i"
if not defined REPO_ROOT (
    echo Not inside a Git working tree: %~dp0
    pause
    exit /b 1
)
cd /d "%REPO_ROOT%"

echo === Working tree before staging ===
git status --short
echo.

echo === Staging 2026-10-28 source and documents ===
git add -- CMakeLists.txt README.md Windows提交命令.bat apps include src proto examples scripts docs/architecture-baseline.md docs/implementation-plan.md 项目架构.md 执行计划.md 2026-10-28-工作日志.md
if errorlevel 1 goto :failed

echo === Staged summary ===
git diff --cached --stat
echo.

set "MSG=%TEMP%\ipc_log_commit_message.txt"
(
echo [feature] Separate demo project and simplify log shutdown
echo.
echo [what] Move the console demo out of apps/ into examples/fdbus-console-demo, keep log_server as the only product entry, replace the old scripts, and keep the internal exit drain without a cross-process flush protocol.
echo [why] The demo and the product were mixed in one CMake target set and one scripts directory, which made the delivery boundary unclear; the previous flush barrier also exposed a business-side API that the assignment does not require.
echo [how] 1. Delete local_log_demo, log_worker and the old demo scripts. 2. Add an independent example CMake project that uses add_subdirectory and links ipc_log_transport. 3. Add build.sh, run_server.sh and run_demo.sh for a three-terminal demo. 4. Keep shutdown-based queue draining in FdbusLogTarget and LogAggregator. 5. Update README, architecture documents, execution plan and the 2026-10-28 daily report.
) > "%MSG%"

git commit -F "%MSG%"
set "COMMIT_STATUS=%ERRORLEVEL%"
del "%MSG%" >nul 2>&1
if not "%COMMIT_STATUS%"=="0" goto :failed

echo.
echo === Commit created ===
git log -1 --stat
echo.
echo Review the commit, then push with:
echo   git push origin main
echo.
pause
exit /b 0

:failed
echo.
echo Commit was not created. Check the output above and git status.
pause
exit /b 1
