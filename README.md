# PERI for OBS Studio

## 主播安装 / Windows x64

已在 OBS Studio 32.1.2 x64 验证。下载并解压 `PERI-OBS-Windows-x64.zip`。从「工具 → 脚本」移除旧的 `peri_publish.py`（如果已加载），完全退出 OBS，再双击 `Install-PERI.cmd`。出现写入权限错误时以管理员身份运行。安装器把插件复制到 Windows 的 ProgramData OBS 插件目录。便携版也可手动将包内的 peri-output.dll 和 peri_obs.dll 放到 OBS 安装目录的 obs-plugins/64bit。

1. 管理员在服务器运行 `./target/release/peri-webtransport obs-token --public-ip 主播可达的IPv4`，私下把整段发布 Token 给主播。Token 包含目标 UDP 地址、资源名、TLS 名称、公开证书和发布凭据。轮换推流码或证书后重新签发。
2. 打开「设置 → 直播」，服务选择 **PERI**，将整段 Token 粘贴进「推流码」。服务器选项使用 Token 内的地址。
3. 在「设置 → 输出 → 直播」选择 AV1，在「设置 → 视频」选择分辨率和帧率，点击主窗口「开始直播」。停止使用同一窗口的「停止直播」。

主播端运行依赖为 OBS 及其编码器。C++ 插件通过 C ABI 调用安装包内的 Rust 动态库 peri_obs.dll。原生回调将编码包放入有界环形缓冲区。网络线程停读时回调直接丢包；丢过视频包后等待关键帧再发布视频，避免传播断裂的参考帧链。普通停播取消网络等待后回收线程。插件沿用 AV1 与 Opus 配置，NVENC 的 B 帧会在本次发布专用实例中禁用，其他不兼容格式回退到 AV1 / Opus。

OBS 32 的直播设置从 rtmp-services 的服务清单读取可选项。插件启动时在当前用户清单中登记 PERI，媒体经 PERI 输出模块发送。内置服务清单更新后，插件会重新合并 PERI 项。卸载时退出 OBS，移除已安装的 peri-output 插件目录即可。

## 开发构建 / Build

将此仓库与 periapsis 仓库并排放置。在安装了 Rust 和 x64 MSVC 的开发者 PowerShell 中运行：

```powershell
./native/build.ps1 -ObsInclude D:/sdk/libobs -ObsLibrary D:/sdk/lib/obs.lib
```

生成的 ZIP 位于本仓库根目录。tests/ring-pressure.cpp 可链接 obs.lib 做独立的拥塞回归测试；它让网络消费者完全停读，并验证回调返回、容量和关键帧恢复。
