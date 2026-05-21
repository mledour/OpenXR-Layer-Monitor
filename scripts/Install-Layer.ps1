# Registers every XR_APILAYER_MLEDOUR_layer_monitor_*.json next to this
# script with the 64-bit OpenXR loader (HKLM). The monitor ships as a
# pre/post pair, so this script registers both manifests in a single run.
# Layer ordering within HKLM\...\Implicit follows insertion order; we
# rely on the host registry tooling to place the target layer between
# pre and post (see README for the manual ordering step).
$RegistryPath = 'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
$JsonPaths = Get-ChildItem -Path $PSScriptRoot -Filter 'XR_APILAYER_MLEDOUR_layer_monitor_*.json' |
    Where-Object { $_.Name -notmatch '-32\.json$' } |
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
