@echo off
echo Pushing catalog to git as digdat0...
cd /d C:\hbupdater
git add romfs/known_repos.json
git -c user.name="digdat0" -c user.email="digdat0@users.noreply.github.com" commit -m "Update catalog"
git push origin main
echo.
if %ERRORLEVEL% EQU 0 (
    echo Done! Catalog pushed to main.
) else (
    echo Push failed — check the output above.
)
pause
