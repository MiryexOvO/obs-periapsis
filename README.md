# OBS PERI 推流

在 OBS「工具 → 脚本」加载同目录下的 peri_publish.py，填写服务器（peri://IP 或 peri://IP:公网UDP端口）和推流码。点击「开始 PERI 推流」，脚本启动 OBS 直播，将节目画面及混音送往 PERI Relay。点击脚本的停止按钮或 OBS 主窗口的「停止直播」结束；编辑字段只影响下一次手动开播。

## 首次连接

使用自签名证书时，在「显示高级设置」中选取从该 Relay 复制的 cert.der。如果服务端使用自定义资源名或证书服务器名，在同一处修改。默认资源 live/av1，证书名 localhost。高级设置也支持旧版邀请链接（服务器栏留空）。

## 主播端运行环境

已验证 Windows x64 的 OBS 32.1.2。保留本仓库的目录结构（包括 native/peri-output.dll），启用 OBS 的 Python 脚本支持。默认从相邻的 periapsis 目录查找 target/release/peri.exe 或 target/debug/peri.exe，使用其中最近构建的版本。更新 DLL 前先退出 OBS。

原生输出模块直接接收 OBS 的 AV1 视频和 Opus 混音编码包，经本机匿名管道交给 peri.exe，再通过 QUIC 发往 Relay。OBS 自带的编码器完成编码，主播端无需另装 FFmpeg 或 PyAV。视频目标码率 20 Mbps，音频 128 kbps。要推 1440p60，请在 OBS「设置 → 视频」选择 2560×1440 的输出分辨率和 60 fps。已验证 NVIDIA NVENC AV1；软件编码的实时性能取决于 CPU。

在脚本面板点击「开始 PERI 推流」。停播可使用 OBS 主窗口的「停止直播」。服务器输出 PERI_RESOURCE_READY 表示收到资源；连接失败时查看主项目的 dist/obs-publish.log 和 OBS 日志。

## 重新构建原生模块 / Rebuild the native module

在 x64 Visual Studio Developer PowerShell 中，为 OBS 32.1.2 的 libobs 头文件目录及 64 位 obs.lib 提供绝对路径，退出 OBS 后从本仓库运行：

```powershell
./native/build.ps1 -ObsInclude D:/sdk/libobs -ObsLibrary D:/sdk/lib/obs.lib
```
