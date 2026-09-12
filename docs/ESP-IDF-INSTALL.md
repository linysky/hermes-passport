# ESP-IDF 安装指南（Windows）

## 方法1：官方安装器（推荐）

1. 下载 ESP-IDF v5.5.3 离线安装器：
   https://github.com/espressif/idf-installer/releases/download/offline-5.5.3/esp-idf-tools-setup-offline-5.5.3.exe
   （约1.6GB）

2. 运行安装器，选择安装目录（默认 `C:\Users\<user>\Desktop\esp-idf`）

3. 安装完成后，桌面会出现 "ESP-IDF 5.5 CMD" 快捷方式

4. 打开 "ESP-IDF 5.5 CMD"，进入项目目录：
   ```
   cd C:\Users\linoo\thinkbeer\hermes-passport
   idf.py set-target esp32c3
   idf.py build
   ```

## 方法2：使用已克隆的仓库

如果已克隆 esp-idf 仓库到 `~/esp/esp-idf-v5.5.3`：

1. 打开 PowerShell（不是 Git Bash）
2. 运行：
   ```powershell
   cd C:\Users\linoo\esp\esp-idf-v5.5.3
   .\install.ps1 esp32c3
   ```

3. 每次打开新终端需要先 source 环境：
   ```powershell
   .\export.ps1
   ```

## 编译项目

```bash
cd ~/thinkbeer/hermes-passport
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor  # 替换 COM3 为实际串口
```

## 已知问题

- Git Bash (MSYS) 不支持 ESP-IDF 工具链，必须用 PowerShell 或 ESP-IDF CMD
- Python 3.14 可能有兼容性问题，建议用 ESP-IDF 自带的 Python 环境
