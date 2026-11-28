<#
Sets up the online server on Fly.io, from this folder, and prints its address.

    powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Name <name>] [-Region <code>]

Needs flyctl (powershell -Command "iwr https://fly.io/install.ps1 -useb | iex") and a
Fly.io account on the Pay As You Go plan (the Free plan has no dedicated IPv4 addresses).
It signs in if needed (a browser opens), makes the app (or uses it if it is already
yours), gives it a dedicated IPv4 address if it has none (UDP needs one; $2 a month at
Fly's prices), and deploys one machine from a folder in TEMP (server.py, the Dockerfile and a
fly.toml made from fly.toml.template), so this folder is never written to.
Run again after an update to deploy the new server; nothing is made twice.
Remove it with: flyctl apps destroy <name>
#>
param([string]$Name = "", [string]$Region = "")
$ErrorActionPreference = "Continue"   # exit codes are checked; 5.1 stops on a native stderr line otherwise
Set-Location $PSScriptRoot

function Find-Fly {
    foreach ($c in "flyctl", "fly") {
        $g = Get-Command $c -ErrorAction SilentlyContinue
        if ($g) { return $g.Source }
    }
    foreach ($p in (Join-Path $HOME ".fly\bin\flyctl.exe"), (Join-Path $HOME ".fly\bin\fly.exe")) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$fly = Find-Fly
if (-not $fly) {
    Write-Host "flyctl is not installed. Install it with:"
    Write-Host '    powershell -Command "iwr https://fly.io/install.ps1 -useb | iex"'
    Write-Host "then open a new terminal and run this again."
    exit 1
}

& $fly auth whoami *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Signing in to Fly.io: a browser opens."
    & $fly auth login
    if ($LASTEXITCODE -ne 0) { Write-Host "Not signed in."; exit 1 }
}

if (-not $Name) {
    $suggest = "timeslip-" + (-join ((48..57) + (97..122) | Get-Random -Count 6 | ForEach-Object { [char]$_ }))
    $Name = Read-Host "A name for the server: letters, digits and dashes, unused on Fly (it becomes name.fly.dev) [$suggest]"
    if (-not $Name) { $Name = $suggest }
}
$Name = $Name.ToLower().Trim()
if ($Name -notmatch '^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$') {
    Write-Host "A name has only letters, digits and dashes, 3 to 63 of them."
    exit 1
}

if (-not $Region) {
    Write-Host "Fly's regions:"
    & $fly platform regions
    $Region = Read-Host "The code of the region nearest you and your friends (like iad, ord, dfw, lax, lhr, fra, syd)"
}
$Region = $Region.ToLower().Trim()
if ($Region -notmatch '^[a-z]{3}$') { Write-Host "A region is a three-letter code from the list."; exit 1 }

# The app: made, or already this account's.
& $fly status -a $Name *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Making the app $Name..."
    & $fly apps create --name $Name --org personal --yes
    if ($LASTEXITCODE -ne 0) { Write-Host "Fly did not make it (is the name taken?)."; exit 1 }
}

# What Fly builds from, in a folder of its own: an installed copy of this folder is left as it is.
$work = Join-Path ([IO.Path]::GetTempPath()) "timeslip-online-server-$Name"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item (Join-Path $PSScriptRoot "server.py"), (Join-Path $PSScriptRoot "Dockerfile") $work -Force
(Get-Content -Raw (Join-Path $PSScriptRoot "fly.toml.template")).Replace("__APP__", $Name).Replace("__REGION__", $Region) |
    Set-Content -Encoding ascii (Join-Path $work "fly.toml")

# A dedicated IPv4 address, once: UDP does not reach an app on the shared one.
$have = $false
try {
    $ips = & $fly ips list -a $Name --json | ConvertFrom-Json
    foreach ($ip in @($ips)) { if ("$($ip.Type)" -eq "v4") { $have = $true } }
} catch { }
if (-not $have) {
    Write-Host "Giving it a dedicated IPv4 address (UDP needs one)..."
    & $fly ips allocate-v4 -a $Name --yes
    if ($LASTEXITCODE -ne 0) { Write-Host "Fly did not give it an address. Dedicated IPv4 addresses need the Pay As You Go plan: on fly.io, Dashboard, Billing, switch to it, then run this again."; exit 1 }
}

Write-Host "Deploying: Fly builds the server and starts one machine (a few minutes the first time)..."
Set-Location $work
& $fly deploy -a $Name -c fly.toml --ha=false --remote-only
if ($LASTEXITCODE -ne 0) { Write-Host "The deploy did not finish; the messages above say why."; exit 1 }

$v4 = ""
try {
    foreach ($ip in @(& $fly ips list -a $Name --json | ConvertFrom-Json)) {
        if ("$($ip.Type)" -eq "v4") { $v4 = "$($ip.Address)" }
    }
} catch { }
Write-Host ""
Write-Host "The online server is running."
Write-Host "In the launcher: Play Together > Connect: Through an online server"
Write-Host "    Server address:  $Name.fly.dev"
if ($v4) { Write-Host "    (or its address, $v4)" }
Write-Host "Everyone playing types the same Server address and Session code."
Write-Host "To remove it, and its cost: flyctl apps destroy $Name"
