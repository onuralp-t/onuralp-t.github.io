# Clean Slate
## Windows
* create: `C:\stuff`, `C:\stuff\PATH`, `C:\github`, `C:\github\<OTHER_ACC_NAME>` ve `C:\github\onuralp-t`
### Remember to download
* Soulseek - peer to peer music sharing platform
* yt-dlp && ffmpeg
* w64devkit https://github.com/skeeto/w64devkit - unbeatable gcc toolchain
### Powershell
* local script yürütmeye izin vermek için powershell terminali açıp komutu yürüt `Set-ExecutionPolicy RemoteSigned -Scope CurrentUser`
*  `notepad $PROFILE` ile `$PROFILE` dosyasını editle, değişiklik yaptıktan sonra dosyayı kaydet, ve yeni terminal aç ya da `. $PROFILE` ile refresh'le
    * `$PROFILE` yoksa `New-Item -ItemType File -Path $PROFILE -Force` yürüt ve tekrar dene
* PROFILE dosyasının içeriği:
```
function git-copy-main{
git fetch origin
git merge origin/main
}

function git-add-commit{
param([string]$commitMessage)
git add .
git commit -m $commitMessage
}

function git-push-to-branch {
param([string]$branchName)
git push -u origin $branchName
}

function git-restore {
git restore .
}

function git-revert {
Write-Host "DANGER: This will permanently remove last commit!" -ForegroundColor Red
$confirmation = Read-Host "Are you sure you want to proceed? (y/n)"

if ($confirmation -ne 'y') {
    Write-Host "Operation cancelled." -ForegroundColor Green
    return
}

Write-Host "Destroying history..." -ForegroundColor Yellow
git reset --hard HEAD~1
git push --force
}

function git-clean-all-history-permanently {
Write-Host "DANGER: This will wipe all commit history!" -ForegroundColor Red
$confirmation = Read-Host "Are you sure you want to proceed? (y/n)"

if ($confirmation -ne 'y') {
    Write-Host "Operation cancelled." -ForegroundColor Green
    return
}

Write-Host "Destroying history..." -ForegroundColor Yellow

git checkout --orphan temp_branch
git add -A
git commit -m "Initial commit"
git branch -D main
git branch -m main
git push -f origin main
}

function git-help{
echo "git-copy-main : "
echo "    git fetch origin"
echo "    git merge origin/main"
echo ""

echo "git-add-commit <commit_message> : "
echo "    git add ."
echo "    git commit -m `"commit_message`""
echo ""

echo "git-push-to-branch <branch_name> : "
echo "    git push -u origin `"branch`""
echo ""

echo "git-restore :  "
echo "    git restore ."
echo ""

echo "git-revert :"
echo "git reset --hard HEAD~1"
echo "git push --force"
echo ""

echo "git-clean-all-history-permanently:"
echo "    git checkout --orphan temp_branch"
echo "    git add -A"
echo "    git commit -m `"Initial commit`""
echo "    git branch -D main"
echo "    git branch -m main"
echo "    git push -f origin main"
}
```

## Github
### Create SSH keys
* create the keys for each acc: (passkey not required)
```
cd ~/.ssh
ssh-keygen -t ed25519 -C "onuralp-t" -f id_ed25519_onuralp-t
ssh-keygen -t ed25519 -C "<OTHER_ACC_NAME>" -f id_ed25519_<OTHER_ACC_NAME>
```
* create .gitconfig and .gitconfig_<OTHER_ACC_NAME> in user's folder, could add other ones as well
```
cd ~/
touch .gitconfig
touch .gitconfig_<OTHER_ACC_NAME>
```
* inside .gitconfig:
```
[user]
    name = onuralp-t
    email = 149798217+onuralp-t@users.noreply.github.com
[core]
    sshCommand = "ssh -i ~/.ssh/id_ed25519_onuralp-t"
[includeIf "gitdir:C:/github/<OTHER_ACC_NAME>/"]
    path = .gitconfig-<OTHER_ACC_NAME>
[init]
	defaultBranch = main
```
* inside .gitconfig_<OTHER_ACC_NAME>:
```
[user]
    name = <OTHER_ACC_NAME>
    email = 15321351+<OTHER_ACC_NAME>@users.noreply.github.com
[core]
    sshCommand = "ssh -i ~/.ssh/id_ed25519_<OTHER_ACC_NAME>"
[init]
	defaultBranch = main
```
* Bu setup için `C:\github`, `C:\github\<OTHER_ACC_NAME>` ve `C:\github\onuralp-t` dosyalarını oluşturdum. global .gitconfig dosyasındaki includeIf satırı sayesinde directory ismine göre farklı configlerin yüklenmesi sağlanabiliyor.
* git attribution kısmında yanlışlıkla kişisel emailini sızdırmamak için Github -> email settings'de github'ın bize assignladığı noreply emailini kopyalayıp buradaki credentials'a ekledim.
* Github'da "private email leakleniyorsa commit'i engelle" ayarını açtım, bu setup ile düzgün çalışıyor.

## VSCode
### VSCode settings
* VSCode Keyboard Shortcuts -> Terminal:Create New == CTRL + N `{ "key": "ctrl+n", "command": "workbench.action.terminal.new"}`
* VSCode -> terminal: Select default profile -> Windows Powershell
### VSCode alias
* save following file as `c.bat` and add it to PATH:
```
@echo off

cd /d %CD%

code .
exit
```
* find `code.cmd` inside VSCode directory, add `start "" ` to the beginning of the `"%~dp0..\Code.exe" "%~dp0..\resources\app\out\cli.js" %*` line to prevent empty terminal opening when `c.bat` is called
* DONE: type `c` in cmd terminal and vscode opens in current dir.
### Code Runner

* [Code Runner](https://marketplace.visualstudio.com/items?itemName=formulahendry.code-runner)
* VSCode Keyboard Shortcuts -> Run Code == CTRL + B `{"key": "ctrl+b", "command": "code-runner.run"}`
* Code Runnor Settings -> Run In Terminal, Ignore Selection, !Show Execution Message, !Show Run Command/Icon *, 
* Code Runnor Settings -> Executor Map settings.json:
```json
"code-runner.executorMap": {
        "c": "cd $dir && if (Test-Path -Path .\\$fileNameWithoutExt.bat) {Invoke-Expression .\\$fileNameWithoutExt.bat} else {coderunnerthingy.bat gcc $fileNameWithoutExt $fileName}",
        "cpp": "cd $dir && if (Test-Path -Path .\\$fileNameWithoutExt.bat) {Invoke-Expression .\\$fileNameWithoutExt.bat} else {coderunnerthingy.bat g++ $fileNameWithoutExt $fileName}",
        }
```
* coderunnerthingy.bat:
```
@echo off

if "%1"=="" (
    echo Please provide a compiler name.
    echo Example: mybat.bat gcc
    goto :eof
)

if "%2"=="" (
    echo Please provide a name after the script name.
    echo Example: mybat.bat Alice
    goto :eof
)

if "%3"=="" (
    echo Please provide a name w ext after the script name.
    echo Example: mybat.bat Alice.c
    goto :eof
)

> %2.bat (
echo @echo off
echo.
echo set before_flags^= -O0 -ggdb3
echo.
echo @echo on
echo %1 %%before_flags%% -o %2.exe %3
echo.
echo .^\%2.exe
)

:eof
```

# Set up a free tier Google Cloud VM
## Quotas and Limits
* https://docs.cloud.google.com/free/docs/free-cloud-features#compute
* https://cloud.google.com/compute/all-pricing
* https://cloud.google.com/vpc/network-pricing?hl=en
## Before creating VM: allowing IPV6 in VPC network
* Google Cloud removed external ipv4 addresses from free tier, but external ipv6 addresses are still allowed for free. (https://cloud.google.com/vpc/network-pricing?hl=en)
* To use external ipv6 addresses, we have to switch to **Premium** Tier Network instead of Standard, which means our general 200 GB egress allowance is down to **1 GB egress** allowence only to specific countries, and we get charged money if some random restricted country pings us.
### Create a new IPv6 enabled VPC network
* create VPC network named "us-east1-ipv6" in auto mode, MTU 1460, routing regional, path selection legacy, uncheck all firewall rules, default settings otherwise
* edit it and change Subnet creation mode to Custom
* edit the auto-generated us-east1 subnet to be dual-stack ipv4 + ipv6 and set ipv6 to external
### Setup Firewall rules of your new VPC network
* allow ::/0 to tcp:22 (ipv6 ssh)
* allow 35.235.240.0/20 to tcp:22 (Google Cloud website SSH button)
* allow ::/0 to tcp:80,443 and udp:443
    * (for ipv6 web traffic, could result in massive egress to restricted countries in form of refused HTTP requests, **COSTS MONEY**) (Need cloudflare to block requests on a per-country basis) (cloudflare requires domain) (use free domain?)
* allow 0.0.0.0/0 to icmp (icmp is important? prolly results in tiny egress to restricted countries)
* allow 10.0.0.0/8 to all protocols (internal google communication, should be safe?)
## Create VM
* WHAT TO SELECT:
    * us-east1
    * Ubuntu 24.04 LTS Minimal
    * No snapshots, no backups
    * 30 GB Standard Persistent Disk
    * Choose previously created VPC network and subnetwork
    * Choose IPv4 and IPv6 (dual-stack)
    * Premium Tier Network (automatically chosen after dual-stack selection)
        * Egress is **Free** to: North America, Europe, (some Asia)
        * Egress is **NOT Free** to **regions**: South America, Middle East, Africa
        * Egress is **NOT Free** to **countries**: Australia, Indonesia, Korea, China, Saudi Arabia
        * source: https://cloud.google.com/vpc/network-pricing
    * Choose External IPv4 address as none, External IPv6 address as Ephemeral (Automatic)
    * Don't install Ops Agent
    * Default settings for security tab and advanced tab

## Github / SSH
* in Ubuntu Linux, generated ssh keys are automatically stored in ssh dotfile in home folder
* /home/<user>/.ssh/id_ed25519.pub

```
gcloud compute instances create ecommerce-vm \
    --project=ecommerce-project-457223 \
    --zone=us-east1-c \
    --machine-type=e2-micro \
    --network-interface=network-tier=STANDARD,stack-type=IPV4_ONLY,subnet=default \
    --maintenance-policy=MIGRATE \
    --provisioning-model=STANDARD \
    --service-account=814607272924-compute@developer.gserviceaccount.com \
    --scopes=https://www.googleapis.com/auth/devstorage.read_only,https://www.googleapis.com/auth/logging.write,https://www.googleapis.com/auth/monitoring.write,https://www.googleapis.com/auth/service.management.readonly,https://www.googleapis.com/auth/servicecontrol,https://www.googleapis.com/auth/trace.append \
    --tags=http-server,https-server \
    --create-disk=auto-delete=yes,boot=yes,device-name=ecommerce-vm,image=projects/debian-cloud/global/images/debian-12-bookworm-v20250415,mode=rw,size=30,type=pd-standard \
    --no-shielded-secure-boot \
    --shielded-vtpm \
    --labels=goog-ec-src=vm_add-gcloud \
    --reservation-affinity=any
```



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