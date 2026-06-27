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
  $classesOpenWithListPath = "$extensionPath\OpenWithList\FlashView.exe"
  $explorerExtensionPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\$extension"
  $explorerOpenWithPath = "$explorerExtensionPath\OpenWithProgids"
  $explorerOpenWithListPath = "$explorerExtensionPath\OpenWithList"
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

  Remove-Item -Recurse -Force $classesOpenWithListPath `
    -ErrorAction SilentlyContinue

  if (Test-Path -LiteralPath $explorerOpenWithPath) {
    Remove-ItemProperty `
      -Path $explorerOpenWithPath `
      -Name $progId `
      -ErrorAction SilentlyContinue
  }

  if (Test-Path -LiteralPath $explorerOpenWithListPath) {
    $openWithListKey = Get-Item -LiteralPath $explorerOpenWithListPath
    $valuesToRemove = @()
    foreach ($valueName in $openWithListKey.GetValueNames()) {
      if ($valueName -eq "MRUList") {
        continue
      }
      $value = $openWithListKey.GetValue($valueName)
      if ($value -is [string] -and $value -ieq "FlashView.exe") {
        $valuesToRemove += $valueName
      }
    }

    foreach ($valueName in $valuesToRemove) {
      Remove-ItemProperty `
        -Path $explorerOpenWithListPath `
        -Name $valueName `
        -ErrorAction SilentlyContinue
    }

    $mruList = $openWithListKey.GetValue("MRUList")
    if ($mruList -is [string] -and $valuesToRemove.Count -gt 0) {
      foreach ($valueName in $valuesToRemove) {
        if ($valueName.Length -eq 1) {
          $mruList = $mruList.Replace($valueName, "")
        }
      }
      Set-ItemProperty `
        -Path $explorerOpenWithListPath `
        -Name "MRUList" `
        -Value $mruList `
        -ErrorAction SilentlyContinue
    }
  }

  Remove-ItemProperty `
    -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts" `
    -Name "${progId}_$extension" `
    -ErrorAction SilentlyContinue
  Remove-ItemProperty `
    -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts" `
    -Name "Applications\FlashView.exe_$extension" `
    -ErrorAction SilentlyContinue
}

Remove-Item -Recurse -Force "HKCU:\Software\Classes\$progId" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force `
  "HKCU:\Software\Classes\Applications\FlashView.exe" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force `
  "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\FlashView.exe" `
  -ErrorAction SilentlyContinue
Remove-ItemProperty `
  -Path "HKCU:\Software\RegisteredApplications" `
  -Name "FlashView" `
  -ErrorAction SilentlyContinue

Write-Host "Unregistered FlashView image file associations."
