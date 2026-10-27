@echo off
setlocal enabledelayedexpansion

echo [1/3] Building md2html...
gcc -std=c23 -O2 -Wall -Wextra -Wpedantic src\md2html.c -o src\md2html.exe
if errorlevel 1 (
    echo Error: Failed to compile md2html.exe
    exit /b 1
)

echo [2/3] Building site_generator...
gcc -std=c23 -O2 -Wall -Wextra -Wpedantic src\site_generator.c -o src\site_generator.exe
if errorlevel 1 (
    echo Error: Failed to compile site_generator.exe
    exit /b 1
)

echo [3/3] Converting markdown articles to HTML fragments...
for /d %%D in (articles\*) do (
    for %%F in ("%%D\*.md") do (
        echo   Converting: %%F -^> %%D\article.html
        src\md2html.exe "%%F" -o "%%D\article.html"
        if errorlevel 1 (
            echo Error: Failed to convert %%F
            exit /b 1
        )
    )
)

echo Assembling single-page index.html...
src\site_generator.exe --articles articles --output index.html
if errorlevel 1 (
    echo Error: Failed to generate index.html
    exit /b 1
)

echo Build complete! index.html is ready.
