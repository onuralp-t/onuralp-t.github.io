* [go back to README](README.md)
# MISC
## Bluetooth headphone stuck in hands free mode - bad audio
* go to bluetooth settings -> remove device
* go to device manager -> view (show hidden) -> remove everything device related in tabs: bluetooth, audio, sound
* mic privacy settings -> disable mic usage system wide (probably superstition)
* reinstall sound drivers
* reinstall bluetooth drivers
* re-pair device
* windows mic settings -> disable mic for the device
* win + r -> run explorer.exe shell:::{A8A91A66-3A7D-4424-8D24-04E180695C7A} to open legacy devices and printers menu
* right click device -> properties -> services -> disable hands free
* mic privacy settings -> reenable mic usage system wide (probably superstition)

## the DOPA script
* download yt-dlp and ffmpeg, add both to PATH.
* save the following file as `dopa.bat` and add it to PATH, use it like `dopa "https://www.youtube.com/watch?v=xZLthhh_UE4"`
```
:: This script requires ffmpeg alongside yt-dlp (for the SRT conversion stuff)

:: usage: dopa.bat "<youtube_link>"
:: e.g.   dopa.bat "https://www.youtube.com/watch?v=np2o6AGRznU"

yt-dlp --skip-download --write-auto-subs --sub-lang ko --convert-subs srt %~1

@echo off
echo.
echo.

echo Text to be copied for the translation context:
echo.

echo The attachment is the automatically generated Korean captions from a Youtube video in SRT format. The video has commentary about League of Legends. Translate the attachment SRT file to English while maintaining SRT format.
```

## set DNS and reset network stuff
* save the following script as `network.bat`
```
@echo off
:: BatchGotAdmin
:-------------------------------------
REM  --> Check for admin permissions
>nul 2>&1 "%SYSTEMROOT%\system32\cacls.exe" "%SYSTEMROOT%\system32\config\system"

REM --> If error flag set, we do not have admin.
if '%errorlevel%' NEQ '0' (
    echo Requesting administrative privileges...
    goto UACPrompt
) else ( goto gotAdmin )

:UACPrompt
    echo Set UAC = CreateObject^("Shell.Application"^) > "%temp%\getadmin.vbs"
    echo UAC.ShellExecute "%~s0", "", "", "runas", 1 >> "%temp%\getadmin.vbs"
    "%temp%\getadmin.vbs"
    exit /B

:gotAdmin
    if exist "%temp%\getadmin.vbs" ( del "%temp%\getadmin.vbs" )
    pushd "%CD%"
    CD /D "%~dp0"

:: ------------------------------------------------------------------------------

@echo on

ipconfig /release
ipconfig /renew
ipconfig /flushdns

netsh interface ip add dns name="Ethernet" addr=1.1.1.1 index=1
netsh interface ip add dns name="Ethernet" addr=1.0.0.1 index=2
netsh interface ipv6 add dnsservers name="Ethernet" address=2606:4700:4700::1111 index=1
netsh interface ipv6 add dnsservers name="Ethernet" address=2606:4700:4700::1001 index=2
netsh interface ip add dns name="Wi-Fi" addr=1.1.1.1 index=1
netsh interface ip add dns name="Wi-Fi" addr=1.0.0.1 index=2
netsh interface ipv6 add dnsservers name="Wi-Fi" address=2606:4700:4700::1111 index=1
netsh interface ipv6 add dnsservers name="Wi-Fi" address=2606:4700:4700::1001 index=2
```