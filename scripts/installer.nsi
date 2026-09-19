Unicode true
!include "MUI2.nsh"
!ifndef VERSION
  !error "VERSION is required"
!endif
Name "Voltura Books"
Caption "Voltura Books ${VERSION} Setup"
BrandingText "Voltura AB"
!define MUI_WELCOMEPAGE_TITLE "Welcome to Voltura Books ${VERSION} Setup"
OutFile "..\dist\VolturaBooks-Setup.exe"
RequestExecutionLevel user
ManifestDPIAware true
ManifestSupportedOS all
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Voltura Books"
VIAddVersionKey /LANG=1033 "CompanyName" "Voltura AB"
VIAddVersionKey /LANG=1033 "FileDescription" "Voltura Books Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "OriginalFilename" "VolturaBooks-Setup-${VERSION}-win-x64.exe"
VIAddVersionKey /LANG=1033 "InternalName" "VolturaBooksSetup"
VIAddVersionKey /LANG=1033 "LegalCopyright" "${U+00A9} 2026 Voltura AB"
VIAddVersionKey /LANG=1033 "Comments" "Developer: Joakim Skoglund; Website: https://voltura.github.io/voltura-books; Address: Voltura AB, H${U+00E4}stholmsv${U+00E4}gen 33, SE-131 71 Nacka, Sweden"
!define MUI_ICON "..\src\book.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\src\installer-banner.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH FitControl
!define MUI_WELCOMEPAGE_TEXT "Install Send to Kindle for books and documents in your Windows account.$\r$\n$\r$\nYour default EPUB reader stays unchanged. Configure your own Kindle address and sender email address after installation."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$LOCALAPPDATA\Programs\Voltura Books\VolturaBooks.exe"
!define MUI_FINISHPAGE_RUN_PARAMETERS "--settings"
!define MUI_FINISHPAGE_RUN_TEXT "Open Voltura Books - Settings"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"
Section "Install"
  SetShellVarContext current
  InitPluginsDir
  SetOutPath "$PLUGINSDIR\package"
  File "..\dist\VolturaBooks.exe"
  File "..\dist\VolturaBooksReader.exe"
  File "..\dist\VolturaBooksDoc.exe"
  File "..\dist\DocSharp.Binary.Doc.dll"
  File "..\dist\DocSharp.Binary.Common.dll"
  File "..\dist\System.IO.Compression.dll"
  File "..\dist\README.md"
  File "..\dist\LICENSE"
  File "..\dist\THIRD-PARTY-NOTICES.txt"
  File "..\dist\uninstall.ps1"
  ExecWait '"$PLUGINSDIR\package\VolturaBooks.exe" --install' $0
  ${If} $0 != 0
    SetErrorLevel 1
    MessageBox MB_OK|MB_ICONSTOP "Installation did not complete. Close Voltura Books if it is open, then run Setup again." /SD IDOK
    Abort
  ${EndIf}
SectionEnd
