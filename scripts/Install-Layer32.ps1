# Registers every XR_APILAYER_MLEDOUR_layer_monitor_*-32.json next to
# this script with the 32-bit OpenXR loader (HKLM\WOW6432Node).
$RegistryPath = 'HKLM:\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit'
$JsonPaths = Get-ChildItem -Path $PSScriptRoot -Filter 'XR_APILAYER_MLEDOUR_layer_monitor_*-32.json' |
    Select-Object -ExpandProperty FullName
$JsonPathList = $JsonPaths -join "`r`n"
Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
	& {
		If (-not (Test-Path '$RegistryPath')) {
			New-Item -Path '$RegistryPath' -Force | Out-Null
		}
		foreach (`$p in @'
$JsonPathList
'@ -split "`r`n") {
			if (`$p) {
				New-ItemProperty -Path '$RegistryPath' -Name `$p -PropertyType DWord -Value 0 -Force | Out-Null
			}
		}
	}
"@
