# OBS PERI 发布

Windows OBS Studio 脚本插件，从 OBS Virtual Camera 读取节目画面，通过 PERI 原生发布端发送 AV1 视频。当前使用 320×180、15 fps，单次最多 13,500 帧，约 15 分钟。观众继续使用现有 PERI 播放器。

## 首次设置

### 服务器和推流码

加载 peri_publish.py，在脚本属性中填写服务器 peri://IP（其他 UDP 端口写 peri://IP:端口），推流码默认 stream。首次在高级设置选择该 Relay 的 cert.der，并核对资源名和证书服务器名。服务器运行 peri-server.py link 可查看两栏；stream-key 可设置新码，重启 Relay 后生效。

### 旧版长邀请

已有工作区的用户，在主项目的发布 PowerShell 窗口生成推流码。窗口须已设置当前 Relay 的 `PERI_PUBLISH_TOKEN`。将下面的 IP 和端口换成你的 Relay 公网地址：

```powershell
python ..\obs-periapsis\peri_link.py 203.0.113.1:4444 localhost .\cert.der live/av1 | Set-Clipboard
```

在 OBS「工具 → 脚本」加载或重新加载 `peri_publish.py`，留空服务器栏，在高级设置的旧版邀请栏粘贴完整的 `peri://v1/…`，点击「开始 PERI 推流」。脚本会启动虚拟摄像机。点击「停止 PERI 推流」会释放本次启动的摄像机。首次加载所需的 OBS Python 和本机编码器见下文。

推流码包含 Relay 地址、资源、TLS 服务器名称、信任证书和发布令牌。编码可逆，持有码的人可以发布。OBS 密码输入框只遮挡显示，保存或导出 OBS 配置时须按密钥保管。观众继续使用独立订阅凭据。

### 手动配置

1. 准备 PERI 工作区：需要 `target/debug/peri.exe`、`dist/live-python` 下的 PyAV，以及导出 `serve(frames)` 的 `scripts/live-av1-worker.py`。插件和主项目放在相邻目录最方便。在主项目的推流终端确认命令行发布可用，随后停止原来的发布者。
2. 在 OBS「工具 → 脚本 → Python 设置」选择该版本 OBS 支持的 64 位 Python 安装目录。回到「脚本」页添加本仓库的 `peri_publish.py`。脚本所用 Python 与下面的编码器解释器可以不同。
3. 在脚本属性设置 PERI 项目目录、Relay 的 IP:端口、证书服务器名、DER 证书、资源名和编码用 Python 的完整路径。发布令牌存放在仓库外、仅当前用户可读的文本文件中，选择该文件。OBS 配置只保存路径。
4. 在 OBS 虚拟摄像机设置选择「节目输出」，勾选脚本的「启动虚拟摄像机时发布到 PERI」，然后启动虚拟摄像机。在 OBS 更换场景即可更换直播内容。停止虚拟摄像机或取消该勾选会停止发布。

## 第一次联调

Relay 输出 `PERI_RESOURCE_READY` 后，确认主项目 `dist/obs-publish.log` 中连续出现 `PERI_ENCODED`，再让观众加入。日志每轮覆盖。如果 DirectShow 打不开 `OBS Virtual Camera`，先确认 OBS 已启动虚拟摄像机，且该设备已注册。脚本不会自动安装驱动。虚拟摄像机模式只传输视频。节目混音模式选用本机 WebM 输出，配置见相邻 PERI 项目的 docs/PERI-PROGRAM-AV.md。
