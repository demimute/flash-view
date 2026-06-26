param(
  [string]$ExePath = ""
)

if ([string]::IsNullOrWhiteSpace($ExePath)) {
  $sameDirectoryExe = Join-Path $PSScriptRoot "fast_viewer.exe"
  $repoBuildExe = Join-Path $PSScriptRoot "..\build\windows-msvc-release\Release\fast_viewer.exe"
  if (Test-Path -LiteralPath $sameDirectoryExe) {
    $ExePath = $sameDirectoryExe
  } else {
    $ExePath = $repoBuildExe
  }
}

$resolvedExe = (Resolve-Path -LiteralPath $ExePath).Path
$progId = "FlashView.Image"
$applicationKey = "HKCU:\Software\Classes\Applications\fast_viewer.exe"
$capabilitiesPath = "$applicationKey\Capabilities"
$appPathsKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\fast_viewer.exe"
$extensions = @(
  ".jpg", ".jpeg", ".png", ".bmp",
  ".gif", ".tif", ".tiff", ".ico", ".webp",
  ".heic", ".heif", ".avif", ".jxl"
)

New-Item -Force -Path $appPathsKey | Out-Null
Set-Item -Path $appPathsKey -Value $resolvedExe

New-Item -Force -Path "HKCU:\Software\Classes\$progId" | Out-Null
Set-Item -Path "HKCU:\Software\Classes\$progId" -Value "FlashView image"

New-Item -Force -Path "HKCU:\Software\Classes\$progId\DefaultIcon" |
  Out-Null
Set-Item -Path "HKCU:\Software\Classes\$progId\DefaultIcon" `
  -Value "`"$resolvedExe`",-102"

New-Item -Force -Path "HKCU:\Software\Classes\$progId\shell\open\command" |
  Out-Null
Set-Item -Path "HKCU:\Software\Classes\$progId\shell\open\command" `
  -Value "`"$resolvedExe`" `"%1`""

New-Item -Force -Path $applicationKey | Out-Null
Set-ItemProperty -Path $applicationKey -Name "FriendlyAppName" `
  -Value "FlashView"
New-Item -Force -Path "$applicationKey\DefaultIcon" | Out-Null
Set-Item -Path "$applicationKey\DefaultIcon" -Value "`"$resolvedExe`",-0"
New-Item -Force -Path "$applicationKey\shell\open\command" | Out-Null
Set-Item -Path "$applicationKey\shell\open\command" `
  -Value "`"$resolvedExe`" `"%1`""
New-Item -Force -Path "$applicationKey\SupportedTypes" | Out-Null

New-Item -Force -Path $capabilitiesPath | Out-Null
Set-ItemProperty -Path $capabilitiesPath -Name "ApplicationName" `
  -Value "FlashView"
Set-ItemProperty -Path $capabilitiesPath -Name "ApplicationDescription" `
  -Value "Fast portable image viewer"
New-Item -Force -Path "$capabilitiesPath\FileAssociations" | Out-Null

New-Item -Force -Path "HKCU:\Software\RegisteredApplications" | Out-Null
Set-ItemProperty -Path "HKCU:\Software\RegisteredApplications" `
  -Name "FlashView" `
  -Value "Software\Classes\Applications\fast_viewer.exe\Capabilities"

foreach ($extension in $extensions) {
  New-Item -Force -Path "HKCU:\Software\Classes\$extension" | Out-Null
  Remove-Item -Recurse -Force `
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\UserChoice" `
    -ErrorAction SilentlyContinue
  Set-Item -Path "HKCU:\Software\Classes\$extension" -Value $progId
  Set-ItemProperty -Path "$capabilitiesPath\FileAssociations" `
    -Name $extension `
    -Value $progId
  Set-ItemProperty -Path "$applicationKey\SupportedTypes" `
    -Name $extension `
    -Value ""

  New-Item -Force -Path "HKCU:\Software\Classes\$extension\OpenWithProgids" |
    Out-Null
  New-ItemProperty `
    -Force `
    -Path "HKCU:\Software\Classes\$extension\OpenWithProgids" `
    -Name $progId `
    -PropertyType Binary `
    -Value ([byte[]]@()) |
    Out-Null
}

Write-Host "Associated FlashView with: $($extensions -join ', ')"
Write-Host "Executable: $resolvedExe"
Write-Host "Archives are intentionally not associated."
