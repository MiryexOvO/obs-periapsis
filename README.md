# OBS PERI 发布

Windows OBS Studio 脚本插件，从 OBS Virtual Camera 读取节目画面，通过 PERI 原生发布端发送 AV1 视频。当前使用 320×180、15 fps，单次最多 13,500 帧，约 15 分钟。观众继续使用现有 PERI 播放器。

## 首次设置

1. 准备 PERI 工作区：需要 `target/debug/peri.exe`、`dist/live-python` 下的 PyAV，以及导出 `serve(frames)` 的 `scripts/live-av1-worker.py`。插件和主项目放在相邻目录最方便。在主项目的推流终端确认命令行发布可用，随后停止原来的发布者。
2. 在 OBS「工具 → 脚本 → Python 设置」选择该版本 OBS 支持的 64 位 Python 安装目录。回到「脚本」页添加本仓库的 `peri_publish.py`。脚本所用 Python 与下面的编码器解释器可以不同。
3. 在脚本属性设置 PERI 项目目录、Relay 的 IP:端口、证书服务器名、DER 证书、资源名和编码用 Python 的完整路径。发布令牌存放在仓库外、仅当前用户可读的文本文件中，选择该文件。OBS 配置只保存路径。
4. 在 OBS 虚拟摄像机设置选择「节目输出」，勾选脚本的「启动虚拟摄像机时发布到 PERI」，然后启动虚拟摄像机。在 OBS 更换场景即可更换直播内容。停止虚拟摄像机或取消该勾选会停止发布。

## 第一次联调

Relay 输出 `PERI_RESOURCE_READY` 后，确认主项目 `dist/obs-publish.log` 中连续出现 `PERI_ENCODED`，再让观众加入。日志每轮覆盖。如果 DirectShow 打不开 `OBS Virtual Camera`，先确认 OBS 已启动虚拟摄像机，且该设备已注册。脚本不会自动安装驱动。此版本只传输视频。
