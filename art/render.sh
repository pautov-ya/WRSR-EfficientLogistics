#!/bin/sh
# Render art/preview.html to art/previewimage.png (1024x1024) with headless Edge.
# Started through PowerShell and waited for: from this shell Edge returns at
# once and may not write the file; a fresh profile each time, so no cached page.
set -e
cd "$(dirname "$0")"
OUT="$(cygpath -w "$PWD/previewimage.png")"
PAGE="file:///$(cygpath -m "$PWD/preview.html")"
powershell.exe -NoProfile -Command "
\$prof = Join-Path \$env:TEMP ('edgeprev' + [guid]::NewGuid().ToString('N'))
\$a = @('--headless=new','--disable-gpu','--hide-scrollbars','--force-device-scale-factor=1',
       \"--user-data-dir=\$prof\",'--window-size=1024,1024','--virtual-time-budget=2000',
       '--screenshot=$OUT','$PAGE')
Start-Process -FilePath 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe' -ArgumentList \$a -Wait
Remove-Item -Recurse -Force \$prof -ErrorAction SilentlyContinue"
echo "[render] art/previewimage.png"
