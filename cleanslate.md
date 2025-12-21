* [go back to README](README.md)
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