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
$extensions = @(
  ".jpg", ".jpeg", ".png", ".bmp",
  ".gif", ".tif", ".tiff", ".ico", ".webp",
  ".heic", ".heif", ".avif", ".jxl"
)

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

$capabilitiesPath = "HKCU:\Software\Classes\Applications\fast_viewer.exe\Capabilities"
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
  Set-Item -Path "HKCU:\Software\Classes\$extension" -Value $progId
  Set-ItemProperty -Path "$capabilitiesPath\FileAssociations" `
    -Name $extension `
    -Value $progId

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

Write-Host "Registered FlashView for: $($extensions -join ', ')"
Write-Host "Executable: $resolvedExe"
Write-Host "Archives are intentionally not associated."
Write-Host "If Windows keeps another default app, choose FlashView once in Settings > Apps > Default apps."
