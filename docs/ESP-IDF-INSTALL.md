<p align="right">
  <strong>English</strong> · <a href="ESP-IDF-INSTALL.zh_CN.md">简体中文</a>
</p>

# ESP-IDF Installation Guide (Windows)

## Method 1: Official installer (recommended)

1. Download the ESP-IDF v5.5.3 offline installer:
   https://github.com/espressif/idf-installer/releases/download/offline-5.5.3/esp-idf-tools-setup-offline-5.5.3.exe
   (about 1.6 GB)

2. Run the installer and choose the install directory (default `C:\Users\<user>\Desktop\esp-idf`)

3. After installation, an "ESP-IDF 5.5 CMD" shortcut appears on the desktop

4. Open "ESP-IDF 5.5 CMD" and enter the project directory:
   ```
   cd C:\Users\linoo\thinkbeer\hermes-passport
   idf.py set-target esp32c3
   idf.py build
   ```

## Method 2: Use a cloned repository

If the esp-idf repository is already cloned to `~/esp/esp-idf-v5.5.3`:

1. Open PowerShell (not Git Bash)
2. Run:
   ```powershell
   cd C:\Users\linoo\esp\esp-idf-v5.5.3
   .\install.ps1 esp32c3
   ```

3. Source the environment in every new terminal:
   ```powershell
   .\export.ps1
   ```

## Build the project

```bash
cd ~/thinkbeer/hermes-passport
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor  # replace COM3 with the actual serial port
```

## Known issues

- Git Bash (MSYS) does not support the ESP-IDF toolchain; use PowerShell or ESP-IDF CMD instead
- Python 3.14 may have compatibility issues; prefer the Python environment bundled with ESP-IDF
