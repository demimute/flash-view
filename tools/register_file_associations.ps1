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
$extensions = @(".jpg", ".jpeg", ".png", ".bmp")

New-Item -Force -Path "HKCU:\Software\Classes\$progId" | Out-Null
Set-Item -Path "HKCU:\Software\Classes\$progId" -Value "FlashView image"

New-Item -Force -Path "HKCU:\Software\Classes\$progId\shell\open\command" |
  Out-Null
Set-Item -Path "HKCU:\Software\Classes\$progId\shell\open\command" `
  -Value "`"$resolvedExe`" `"%1`""

foreach ($extension in $extensions) {
  New-Item -Force -Path "HKCU:\Software\Classes\$extension" | Out-Null
  Set-Item -Path "HKCU:\Software\Classes\$extension" -Value $progId

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
