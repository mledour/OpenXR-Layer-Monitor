; installer.iss -- Inno Setup script for the XR_APILAYER_MLEDOUR_layer_monitor
; sandwich (pre + post pair).
;
; Builds a single-file Setup.exe that:
;   1. Copies both DLLs + both JSON manifests to Program Files (correct ACLs
;      for sandboxed identities like WebXR in Chrome, inherited by default).
;   2. Registers BOTH JSON manifests in HKLM for the OpenXR loader.
;   3. Creates an Add/Remove Programs entry with uninstaller.
;
; Compile from CI or locally:
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DMyAppVersion=0.0.1 installer.iss
;
; The /DMyAppVersion flag is mandatory for tagged builds; for local dev
; builds without it, the fallback "0.0.0-dev" is used.

#define MyAppName "XR_APILAYER_MLEDOUR_layer_monitor"
#define MyAppPre  "XR_APILAYER_MLEDOUR_layer_monitor_pre"
#define MyAppPost "XR_APILAYER_MLEDOUR_layer_monitor_post"

; Accept version from the ISCC command line (/DMyAppVersion=x.y.z).
; Fall back to a dev placeholder when compiling interactively.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

[Setup]
; AppId is a fixed GUID that identifies this product across upgrades.
; The monitor pair shares one AppId because they ship as a single unit;
; installing them separately is not supported (a pre without a post is
; a no-op sandwich, so the installer always lays down both).
AppId={{25EB8E51-53A8-465F-8456-4E6446055DCB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Michael Ledour
AppPublisherURL=https://github.com/mledour/OpenXR-Layer-Monitor
AppSupportURL=https://github.com/mledour/OpenXR-Layer-Monitor/issues
DefaultDirName={autopf}\OpenXR-Layer-Monitor
; No Start Menu group -- this layer has no user-facing executable.
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\bin\installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-x64-Setup
Compression=lzma2
SolidCompression=yes
; x64 only. The layer DLLs are 64-bit; 32-bit is not currently shipped.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
; Admin elevation required: writing to HKLM + Program Files.
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; Both halves of the sandwich, side by side in the install dir. Paths are
; relative to this .iss file.
Source: "..\bin\x64\Release\{#MyAppPre}.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\x64\Release\{#MyAppPre}.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\x64\Release\{#MyAppPost}.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\x64\Release\{#MyAppPost}.json"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Register BOTH manifests as implicit API layers for the OpenXR 1.x loader.
; The value name is the full path to the JSON manifest; the DWORD value 0
; means "enabled" (the loader spec treats non-zero as "disabled"). Flags:
; uninsdeletevalue removes the entry automatically on uninstall.
;
; The OpenXR loader walks ApiLayers\Implicit in insertion order. Inno Setup
; writes registry entries in declaration order; on a clean install this puts
; pre before post. If the target layer sits between them in the loader's
; ordering, the sandwich captures it correctly. Users can verify with:
;   reg query HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit
Root: HKLM; Subkey: "Software\Khronos\OpenXR\1\ApiLayers\Implicit"; \
  ValueName: "{app}\{#MyAppPre}.json"; ValueType: dword; ValueData: 0; \
  Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Khronos\OpenXR\1\ApiLayers\Implicit"; \
  ValueName: "{app}\{#MyAppPost}.json"; ValueType: dword; ValueData: 0; \
  Flags: uninsdeletevalue
