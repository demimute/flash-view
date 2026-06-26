param()

$progId = "FlashView.Image"
$extensions = @(
  ".jpg", ".jpeg", ".png", ".bmp",
  ".gif", ".tif", ".tiff", ".ico", ".webp",
  ".heic", ".heif", ".avif", ".jxl"
)

foreach ($extension in $extensions) {
  $extensionPath = "HKCU:\Software\Classes\$extension"
  $openWithPath = "$extensionPath\OpenWithProgids"
  $userChoicePath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension\UserChoice"

  if (Test-Path -LiteralPath $extensionPath) {
    $defaultValue = (Get-Item -LiteralPath $extensionPath).GetValue("")
    if ($defaultValue -eq $progId) {
      Set-Item -Path $extensionPath -Value ""
    }
  }

  if (Test-Path -LiteralPath $userChoicePath) {
    $userChoiceProgId = (Get-ItemProperty -LiteralPath $userChoicePath `
      -Name "ProgId" `
      -ErrorAction SilentlyContinue).ProgId
    if ($userChoiceProgId -eq $progId) {
      Remove-Item -Recurse -Force $userChoicePath `
        -ErrorAction SilentlyContinue
    }
  }

  if (Test-Path -LiteralPath $openWithPath) {
    Remove-ItemProperty `
      -Path $openWithPath `
      -Name $progId `
      -ErrorAction SilentlyContinue
  }
}

Remove-Item -Recurse -Force "HKCU:\Software\Classes\$progId" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force `
  "HKCU:\Software\Classes\Applications\fast_viewer.exe" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force `
  "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\fast_viewer.exe" `
  -ErrorAction SilentlyContinue
Remove-ItemProperty `
  -Path "HKCU:\Software\RegisteredApplications" `
  -Name "FlashView" `
  -ErrorAction SilentlyContinue

Write-Host "Unregistered FlashView image file associations."
