# PERI for OBS Studio

## 主播安装 / Windows x64

已在 OBS Studio 32.1.2 x64 验证。下载并解压 `PERI-OBS-Windows-x64.zip`。从「工具 → 脚本」移除旧的 `peri_publish.py`（如果已加载），完全退出 OBS，再双击 `Install-PERI.cmd`。出现写入权限错误时以管理员身份运行。安装器把插件复制到 Windows 的 ProgramData OBS 插件目录。便携版也可手动将包内的 peri-output.dll 和 peri_obs.dll 放到 OBS 安装目录的 obs-plugins/64bit。

1. 管理员在服务器运行 `./target/release/peri-webtransport link`，私下交给主播输出的服务器地址和推流码。首次部署使用 `setup`；已有部署修改连接地址使用 `configure`，不用编辑 JSON。
2. 打开「设置 → 直播」，服务选择 **PERI**。在服务器栏直接输入 `peri://IP:端口`，在推流码栏粘贴 `peri1_` 开头的推流码，点击应用。地址和端口可独立修改，无需重新生成推流码。
3. 在「设置 → 输出 → 直播」选择 AV1，在「设置 → 视频」选择分辨率和帧率，点击主窗口「开始直播」。停止使用同一窗口的「停止直播」。

例如直连服务器填写 `peri://192.0.2.10:4444`，有端口映射时填写主播实际连接的外部地址，如 `peri://43.248.128.243:23927`。IPv6 使用 `peri://[IPv6地址]:端口`。当前入口要求 IP 字面量，不接受域名或附加路径。主播无需下载或安装证书；推流码自带服务器身份指纹，插件在发送凭据前验证身份。它是私密的机器生成凭据，不是可随意填写的短口令。

旧 `peri://v1/…` Token 继续兼容：服务器栏保留 `peri://invitation` 时使用旧 Token 内地址，填写明确的 `peri://IP:端口` 时覆盖旧地址。证书或发布凭据轮换后需重新取得推流码；单纯修改 IP/端口不需要轮换。此连接简化只作用于 OBS 发布，浏览器 WebTransport 的 HTTPS 要求不变。

可编辑服务器栏由插件扩展 OBS 32.1.2 的原设置页实现，不修改 OBS 安装文件；其他服务的输入方式保持原样。更新时请完全退出 OBS，再成套替换两个 DLL。

主播端运行依赖为 OBS 及其编码器。C++ 插件通过 C ABI 调用安装包内的 Rust 动态库 peri_obs.dll。原生回调将编码包放入有界环形缓冲区。网络线程停读时回调直接丢包；丢过视频包后等待关键帧再发布视频，避免传播断裂的参考帧链。普通停播取消网络等待后回收线程。插件沿用 AV1 与 Opus 配置，NVENC 的 B 帧会在本次发布专用实例中禁用，其他不兼容格式回退到 AV1 / Opus。

主动关键帧恢复目前限 OBS 32.1.2 的 `obs_nvenc_av1_tex`、CBR 配置。插件复制直播编码器，保留原档案与录制编码器；独立反馈线程接收下游恢复请求及本地队列丢帧通知，合并请求并以至少 333 ms 间隔提交原设置热更新。实际关键帧到达前不视为编码响应完成；1 秒未观察到关键帧会记录警告。其他版本、编码器或码率模式继续等待周期关键帧，不宣称支持主动恢复。请成套更新 ZIP 内的两个 DLL。

Active keyframe recovery is limited to OBS 32.1.2, `obs_nvenc_av1_tex`, and CBR. A dedicated streaming encoder preserves saved settings and recording outputs. Feedback runs independently of media writes, coalesces requests, and submits unchanged-settings updates at least 333 ms apart. A submitted update is not a confirmed keyframe; missing responses are logged after one second. Other configurations retain periodic-keyframe recovery. Update both DLLs together.

队列最多 32 包且已占用数据不超过 2 MiB（预分配容量不等于已占用数据）。默认继续使用 5 秒发送预算。仅在隔离实验环境设置 `PERI_OBS_UPLINK_EXPERIMENT=1` 时，启用 750 ms 排队/发送/确认总预算及本地过载码率退让；握手与目录保持独立的 5 秒预算。配套 Relay 必须支持发布端 `RESET(1)` 局部取消语义，请同步更新 Relay 与两个 DLL。此策略尚未通过弱网播放验收：本机曾出现请求目标 4 Mbps、实际编码仍约 26 Mbps，不能把更新日志当作码率控制生效。`PERI_OBS_DIAGNOSTICS=1` 可记录实际编码字节速率。

The queue holds at most 32 packets and 2 MiB of occupied data; reserved allocation is separate. Default sends retain the five-second budget. Only isolated experiments with `PERI_OBS_UPLINK_EXPERIMENT=1` enable a 750 ms queue/write/receipt deadline and local-overload bitrate requests. Update the Relay alongside both DLLs for publisher RESET(1) support. Impaired playback acceptance has not passed: a requested 4 Mbps target still produced about 26 Mbps locally. Request logs do not prove effective adaptation. `PERI_OBS_DIAGNOSTICS=1` records encoded-byte rates.

后续定位确认：静止/平移纹理下的 NVENC 热更新有效，约 26 Mbps 的异常出现在逐帧独立随机纹理；不能据此认定 OBS 更新接口普遍失效。当前实验还包含 32 对象 / 2 MiB 在途信用、最低 1 Mbps 的过载退让、实际输出能力判定和恢复请求代数合并。队列、在途数据、编码器与 QUIC 内存是不同预算。

实验 Relay 需同时设置 `PERI_UNI_STREAM_WINDOW=32` 才能提供对应的 QUIC 开流信用；否则对端默认八条入站流仍会限制发布端。连接接收字节上限保持 8 MiB。浏览器实验脚本会为上行实验自动选择此值并写入报告，正式 Relay 默认仍为八条。

配套实验 Relay 使用 64 帧分发队列与每观众 2 MiB 负载信用，保持 300 ms 驻留期限。网关局部对象取消改为等待关键帧恢复，不直接重建整条订阅。OBS 排队仍为 32 槽；扩大到 64 槽的对照增加了延迟，未保留。

在上行实验基础上额外设置 `PERI_OBS_KEY_ADMISSION=1`，可启用尚未通过稳定性验收的关键帧接纳候选：预留一个槽和最大合法关键帧的字节额度，普通包最多占31槽；总上限仍32槽/2MiB。发生拥塞后，仅由写线程跳过更新关键帧之前的旧排队前缀，保持原750ms期限，正在执行的网络调用不会被回调抢占。该候选独立默认关闭；移除变量即保留既有FIFO路径。重复弱网测试未稳定通过，不能据单轮通过宣称收益。诊断模式可逐关键帧区分编码、接纳/拒绝、开始发送和调用结果；编码回调只写有界事件队列，诊断溢出会明确报告。

在同一个上行实验环境中，可额外设置 `PERI_OBS_ENCODER_BUDGET=1` 限制编码突发，`PERI_OBS_CC=bbr` 对照 BBR（默认 Cubic），或 `PERI_OBS_PROFILE=720p30` / `360p30` 选择下次启动的独立编码档位，码率分别不超过 4 Mbps / 1 Mbps。档位不放大画面，不修改 OBS 保存配置或录制编码器，也不代表已支持直播中自动切换分辨率。实验能力限上述 OBS/NVENC/CBR 组合，BBR 公平性与公网稳定性尚未验收。关闭这些开关应移除对应环境变量；当前实现按变量存在启用，字符串 `0` 不是关闭。

Correlated-content bitrate updates work; independent random textures exposed output overshoot. Additional opt-in controls are encoder burst budgeting (`PERI_OBS_ENCODER_BUDGET=1`), BBR comparison (`PERI_OBS_CC=bbr`), and next-session profiles (`PERI_OBS_PROFILE=720p30` or `360p30`, capped at 4 or 1 Mbps). Profiles affect only the dedicated PERI encoder. Runtime resolution switching and public-network/fairness acceptance are not claimed. Remove environment variables to disable these presence-based switches.

OBS 32 的直播设置从 rtmp-services 的服务清单读取可选项。插件启动时在当前用户清单中登记 PERI，媒体经 PERI 输出模块发送。内置服务清单更新后，插件会重新合并 PERI 项。卸载时退出 OBS，移除已安装的 peri-output 插件目录即可。

## 开发构建 / Build

将此仓库与 periapsis 仓库并排放置。在安装了 Rust 和 x64 MSVC 的开发者 PowerShell 中运行：

```powershell
./native/build.ps1 -ObsInclude D:/sdk/libobs -ObsLibrary D:/sdk/lib/obs.lib -Qt D:/sdk/qt6
```

生成的 ZIP 位于本仓库根目录。tests/ring-pressure.cpp 可链接 obs.lib 做独立的拥塞回归测试；它让网络消费者完全停读，并验证回调返回、容量和关键帧恢复。

设置页扩展需与 OBS 匹配的 Qt 开发包；本次使用 OBS 官方 [2025-08-23 依赖包](https://github.com/obsproject/obs-deps/releases/tag/2025-08-23) 中的 Windows x64 Qt 6.8.3。运行时复用 OBS 自带的 Qt DLL，不向安装包复制另一套 Qt。默认 SDK 路径为相邻仓库 `target/obs-sdk/qt6`。`settings-ui.cpp` 回归覆盖地址编辑、保存数据、重新加载及其他服务隔离。

`tests/build-check.ps1` 构建插件和原生回归夹具（默认使用本机 `D:/VS`、`D:/Windows Kits/10` 及相邻项目 `target/obs-sdk`，可配置路径）。在相邻 periapsis 仓库执行 `cargo build --release --locked -p peri-native --example obs_recovery`，再执行 `node ../obs-periapsis/tests/recovery-check.cjs`，可用隔离的真实 libobs/NVENC、Relay 与 FFmpeg 验证恢复 GOP。测试将周期关键帧设为 30 秒，先观察 1 秒无请求对照，再请求恢复并独立解码 30 帧；结果保存在 periapsis/dist。

The native checks use an isolated real libobs/NVENC source, a local Relay, and FFmpeg decoding. A 30-second periodic GOP plus a one-second no-request control distinguishes active recovery from periodic keys. These checks measure local request-to-key delivery, not public-network latency.

## 1.0.0 测试发布

配套 PERI 1.0.0，OBS 32.1.2 / Windows x64。正式资产名称为 `PERI-OBS-1.0.0-Windows-x64.zip`；包内 VERSION 标明版本，两个 DLL 必须成套替换。协议保持现有 ALPN 与封装。安装前退出 OBS，输入自由编辑的 `peri://IP:端口` 与独立推流码。

插件模块与其 Rust 客户端库采用 BSD-3-Clause，见 LICENSE；服务端 Relay 独立采用 AGPL-3.0-only，协议规范文档采用 Apache-2.0。公开分发时同时提供匹配的源码包及许可证。服务端升级和回滚步骤见配套核心仓库 `docs/RELEASE-1.0.0.md`。
