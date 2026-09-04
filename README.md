# FoxMes Desktop

FoxMes Desktop is built on the Telegram Desktop core.

## Running an unsigned download

FoxMes Desktop 1.0.0 is distributed without a commercial code-signing certificate. Before bypassing an operating-system warning, verify the downloaded file against `SHA256SUMS` from the same GitHub release.

### macOS

Copy `FoxMes.app` from the DMG to the `Applications` folder. Then open Terminal and run:

```bash
xattr -dr com.apple.quarantine "/Applications/FoxMes.app"
codesign --force --deep --sign - "/Applications/FoxMes.app"
open "/Applications/FoxMes.app"
```

Alternatively, try to open FoxMes once, then go to **System Settings → Privacy & Security** and select **Open Anyway**.

### Windows

Right-click the downloaded installer, select **Properties**, enable **Unblock** if that option is shown, and click **Apply**. When Microsoft Defender SmartScreen appears, select **More info → Run anyway**.

The file can also be unblocked with PowerShell:

```powershell
Unblock-File .\FoxMes-1.0.0-windows-x64-setup.exe
```

For the portable version, unblock the ZIP before extracting it:

```powershell
Unblock-File .\FoxMes-1.0.0-windows-x64-portable.zip
Expand-Archive .\FoxMes-1.0.0-windows-x64-portable.zip .\FoxMes-portable
.\FoxMes-portable\FoxMes.exe
```

### Linux

For the AppImage, grant execute permission and start it:

```bash
chmod +x FoxMes-1.0.0-linux-x86_64.AppImage
./FoxMes-1.0.0-linux-x86_64.AppImage
```

If AppImage mounting is unavailable, extract and run it directly:

```bash
./FoxMes-1.0.0-linux-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun
```

For the tar archive:

```bash
mkdir FoxMes
tar -xJf FoxMes-1.0.0-linux-x86_64.tar.xz -C FoxMes
./FoxMes/AppRun
```

## License

The source code is licensed under GNU GPLv3 with the OpenSSL exception. See [LICENSE](LICENSE) and [LEGAL](LEGAL). Telegram Desktop copyright, license notices, and required upstream attribution are retained.
