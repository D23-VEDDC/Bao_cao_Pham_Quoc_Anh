param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('build', 'flash', 'monitor', 'menuconfig')]
    [string] $Action,
    [string] $Port
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$settings = Get-Content (Join-Path $projectRoot '.vscode/settings.json') -Raw | ConvertFrom-Json
$idfRoot = $settings.'idf.currentSetup'
$toolsRoot = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif' }
$installConfig = Get-Content (Join-Path $toolsRoot 'esp_idf.json') -Raw | ConvertFrom-Json
$normalizedIdfRoot = $idfRoot.Replace('\', '/').TrimEnd('/')
$installation = $installConfig.idfInstalled.PSObject.Properties |
    Where-Object { $_.Value.path.Replace('\', '/').TrimEnd('/') -ieq $normalizedIdfRoot } |
    Select-Object -First 1
if (-not $installation) {
    throw "ESP-IDF installation not found for $idfRoot in $toolsRoot\esp_idf.json"
}

$python = $installation.Value.python
$env:IDF_TOOLS_PATH = $toolsRoot
$env:PATH = "$(Split-Path $python -Parent);$(Split-Path $installConfig.gitPath -Parent);$env:PATH"
. (Join-Path $idfRoot 'export.ps1')
if (-not $env:IDF_PATH) { throw 'ESP-IDF environment activation failed' }

$idfArgs = @()
if ($Port) { $idfArgs += @('-p', $Port) }
$idfArgs += $Action
& $python (Join-Path $idfRoot 'tools/idf.py') @idfArgs
exit $LASTEXITCODE
