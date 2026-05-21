$JsonPaths = Get-ChildItem -Path $PSScriptRoot -Filter 'XR_APILAYER_MLEDOUR_layer_monitor_*.json' |
    Where-Object { $_.Name -notmatch '-32\.json$' } |
    Select-Object -ExpandProperty FullName
$JsonPathList = $JsonPaths -join "`r`n"
Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
	& {
		foreach (`$p in @'
$JsonPathList
'@ -split "`r`n") {
			if (`$p) {
				Remove-ItemProperty -Path 'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit' -Name `$p -Force -ErrorAction SilentlyContinue
			}
		}
	}
"@
