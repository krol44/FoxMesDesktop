FoxMes Desktop 1.9.5. This is the first cross-platform FoxMes Desktop release for Windows x64, macOS 13+ on Apple silicon, and Linux x86_64.

## Removing the quarantine

### macOS

Copy `FoxMes.app` from the DMG to `Applications`, then run in Terminal:

```bash
xattr -dr com.apple.quarantine "/Applications/FoxMes.app"
```

### Windows

Right-click the downloaded file, select **Properties**, enable **Unblock** and click **Apply**. Or in PowerShell, in the download folder:

```powershell
Unblock-File .\FoxMes-*-windows-x64-*
```

If SmartScreen still appears, select **More info → Run anyway**.

### Linux

```bash
chmod +x FoxMes-*-linux-x86_64.AppImage
```
