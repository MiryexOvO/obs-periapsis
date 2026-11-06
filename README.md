# OBS PERI 推流

在 OBS「工具 → 脚本」加载同目录下的 peri_publish.py，填写服务器（peri://IP 或 peri://IP:公网UDP端口）和推流码。点击「开始 PERI 推流」，脚本启动 OBS 直播，将节目画面及混音送往 PERI Relay。点击脚本的停止按钮或 OBS 主窗口的「停止直播」结束；编辑字段只影响下一次手动开播。

## 首次连接

使用自签名证书时，在「显示高级设置」中选取从该 Relay 复制的 cert.der。如果服务端使用自定义资源名或证书服务器名，在同一处修改。默认资源 live/av1，证书名 localhost。高级设置也支持旧版邀请链接（服务器栏留空）。

## 主播端运行环境

Windows OBS 需启用与其版本匹配的 Python 脚本支持。默认主项目位于相邻的 periapsis 目录，需包含 target/debug/peri.exe、scripts/program-av.py、dist/live-python 下匹配编码用 Python 版本的 PyAV。FFmpeg 需支持 RTMP 输入、libaom-av1 和 libopus；可用 PERI_FFMPEG 环境变量指定。OBS 直播编码请选择常见的 H.264 和 AAC。

插件开播时临时切换 OBS 至回环输入，停播时恢复原直播服务。本机将输入转为最高 1280×720、30 fps AV1 与双声道 Opus，经 QUIC 发送至服务器。服务器输出 PERI_RESOURCE_READY 表示接收成功。失败后在 dist/obs-publish.log 查看本次原因，修正后可再次点击开始。
