[Setup]
AppId=org.hopview.desktop
AppName=Hopview
AppVersion=0.6.0
DefaultDirName={localappdata}\Programs\Hopview
DefaultGroupName=Hopview
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\hopview.exe
SetupIconFile=hopview.ico
LicenseFile=..\LICENSE
OutputDir=..\build-native
OutputBaseFilename=Hopview-Windows-x64
Compression=lzma2
SolidCompression=yes

[Files]
Source: "..\build-native\stage\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Hopview"; Filename: "{app}\hopview.exe"
