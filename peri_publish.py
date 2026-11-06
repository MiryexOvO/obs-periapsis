"""Start an OBS program broadcast through PERI. / 一键发布 OBS 节目画面与混音。"""
import os
import runpy
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import obspython as obs

plugin_dir = Path(__file__).resolve().parent
links = runpy.run_path(str(plugin_dir / "peri_link.py"))
parse_link = links["parse"]
config = {}
publisher = None
bridge = None
session_dir = None
previous_service = None
phase = "idle"
deadline = 0.0
local_port = 0
log_path = None


ENCODE_ARGS = (
    "-vf", "scale=w='min(1280,iw)':h='min(720,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2,fps=30",
    "-c:v", "libaom-av1", "-usage", "realtime", "-cpu-used", "8",
    "-lag-in-frames", "0", "-g", "30", "-b:v", "1500k", "-pix_fmt", "yuv420p",
    "-c:a", "libopus", "-frame_duration", "20", "-ar", "48000", "-ac", "2", "-f", "nut",
    "-flush_packets", "1",
)
working_ffmpeg = None


def select_ffmpeg():
    global working_ffmpeg
    if working_ffmpeg:
        return working_ffmpeg
    override = os.environ.get("PERI_FFMPEG")
    candidates = [Path(override)] if override else [
        Path(entry.strip('"')) / "ffmpeg.exe" for entry in os.get_exec_path() if entry
    ]
    if not override:
        candidates.append(Path.home() / "scoop/shims/ffmpeg.exe")
    # Exercise the exact live filters, codecs and muxer before starting OBS.
    # 开播前用相同滤镜、编码器与封装做短测，跳过同名但不兼容的程序。
    for path in dict.fromkeys(candidates):
        if not override and not path.is_file():
            continue
        try:
            probe = subprocess.run([
                str(path), "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=s=64x64:r=30",
                "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", "0.1",
                *ENCODE_ARGS, "pipe:1"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW, timeout=4)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if probe.returncode == 0:
            working_ffmpeg = str(path)
            obs.script_log(obs.LOG_INFO, f"PERI 编码器已验证：{path}")
            return working_ffmpeg
    raise ValueError("未找到通过 AV1/Opus 编码测试的 FFmpeg，请检查 PERI_FFMPEG 或安装完整版 FFmpeg")


def script_description():
    return "填写服务器和推流码，点击开始即可直播。首次连接请在高级设置选择服务器证书。"


def script_defaults(settings):
    for key, value in (("stream_key", "stream"), ("address", "127.0.0.1:4444"), ("name", "localhost"),
                       ("resource", "live/av1"), ("core", str(plugin_dir.parent / "periapsis")), ("cert", str(plugin_dir.parent / "periapsis/cert.der")),
                       ("python", os.environ.get("PERI_PYTHON", "python"))):
        obs.obs_data_set_default_string(settings, key, value)


def script_properties():
    props = obs.obs_properties_create()
    obs.obs_properties_add_text(props, "server", "服务器（peri://IP）", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_text(props, "stream_key", "推流码", obs.OBS_TEXT_PASSWORD)
    obs.obs_properties_add_button(props, "start", "开始 PERI 推流", begin)
    obs.obs_properties_add_button(props, "stop", "停止 PERI 推流", end)
    toggle = obs.obs_properties_add_bool(props, "show_advanced", "显示高级设置")
    obs.obs_property_set_modified_callback(toggle, show_advanced)
    advanced = obs.obs_properties_create()
    obs.obs_properties_add_text(advanced, "link", "旧版邀请链接", obs.OBS_TEXT_PASSWORD)
    for key, label in (("name", "证书服务器名"),
                       ("resource", "资源名"), ("python", "编码用 Python 路径")):
        obs.obs_properties_add_text(advanced, key, label, obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_path(advanced, "core", "PERI 项目目录", obs.OBS_PATH_DIRECTORY, None, None)
    obs.obs_properties_add_path(advanced, "cert", "服务器证书 (.der)", obs.OBS_PATH_FILE, "*.der", None)
    group = obs.obs_properties_add_group(props, "advanced", "高级设置（首次证书与本机路径）", obs.OBS_GROUP_NORMAL, advanced)
    obs.obs_property_set_visible(group, config.get("show_advanced", False))
    return props


def show_advanced(props, prop, settings):
    obs.obs_property_set_visible(obs.obs_properties_get(props, "advanced"),
                                 obs.obs_data_get_bool(settings, "show_advanced"))
    return True


def script_update(settings):
    config["show_advanced"] = obs.obs_data_get_bool(settings, "show_advanced")
    # Editing settings only prepares the next broadcast. / 编辑设置仅影响下次开播。
    for key in ("server", "stream_key", "link", "core", "address", "name", "resource", "python", "cert", "token"):
        config[key] = obs.obs_data_get_string(settings, key)


def begin(props, prop):
    global publisher, bridge, session_dir, phase, deadline, local_port, log_path
    if phase != "idle":
        return False
    if obs.obs_frontend_streaming_active():
        obs.script_log(obs.LOG_WARNING, "OBS 已有直播，请先结束当前直播。")
        return False
    try:
        root = Path(config["core"]).resolve()
        address, name, resource, cert = (config[k] for k in ("address", "name", "resource", "cert"))
        if config["server"].strip():
            address, token = links["short_settings"](config["server"], config["stream_key"])
            if not Path(cert).is_file():
                raise ValueError("请在高级设置选择服务器的 cert.der")
        elif config["link"].strip():
            address, name, resource, token, der = parse_link(config["link"].strip())
            session_dir = tempfile.TemporaryDirectory(prefix="peri-obs-")
            cert = str(Path(session_dir.name) / "cert.der")
            Path(cert).write_bytes(der)
        else:
            raise ValueError("请先填写服务器地址 peri://IP 和推流码")
        if not (32 <= len(token) <= 128 and all("!" <= c <= "~" for c in token)):
            raise ValueError("发布令牌文件格式无效")
        env = dict(os.environ, PERI_PUBLISH_TOKEN=token, PERI_ENCODER_SCRIPT=str(root / "scripts/live-av1-worker.py"),
                   PERI_PYTHON=config["python"], PYTHONPATH=str(root / "dist/live-python"))
        args = [config["python"], str(root / "scripts/program-av.py"), "publish",
                address, name, cert, resource, "--peri", str(root / "target/debug/peri.exe"), "--stdin-program"]
        log_path = root / "dist/obs-publish.log"
        log_path.parent.mkdir(exist_ok=True)
        # Use a fresh loopback port per broadcast. / 每次开播选用新的本机回环端口。
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            local_port = probe.getsockname()[1]
        with log_path.open("wb") as output:
            ffmpeg = select_ffmpeg()
            bridge = subprocess.Popen([
                ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error",
                "-listen", "1", "-i", f"rtmp://127.0.0.1:{local_port}/live/peri",
                "-map", "0:v:0", "-map", "0:a:0",
                *ENCODE_ARGS, "pipe:1"],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=output,
                creationflags=subprocess.CREATE_NO_WINDOW)
            source = bridge.stdout
            try:
                publisher = subprocess.Popen(args, cwd=root, env=env, stdout=output,
                                             stderr=subprocess.STDOUT, stdin=source,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
            finally:
                if bridge and bridge.stdout:
                    bridge.stdout.close()
        phase, deadline = "arming", time.monotonic() + 10
    except (OSError, ValueError, KeyError) as error:
        stop()
        obs.script_log(obs.LOG_ERROR, str(error))
    return False


def start_stream():
    global previous_service, phase, deadline
    settings = obs.obs_data_create()
    try:
        obs.obs_data_set_string(settings, "server", f"rtmp://127.0.0.1:{local_port}/live")
        obs.obs_data_set_string(settings, "key", "peri")
        service = obs.obs_service_create("rtmp_custom", "PERI local program", settings, None)
    finally:
        obs.obs_data_release(settings)
    if not service:
        raise RuntimeError("OBS 无法创建本机直播输出")
    try:
        # Retain the borrowed service until we restore it after stopping.
        # 暂存 OBS 原服务引用，停播后恢复。
        current = obs.obs_frontend_get_streaming_service()
        if current:
            previous_service = obs.obs_service_get_ref(current)
        obs.obs_frontend_set_streaming_service(service)
    finally:
        obs.obs_service_release(service)
    phase, deadline = "starting", time.monotonic() + 20
    obs.obs_frontend_streaming_start()


def poll():
    if phase in ("idle", "stopping"):
        return
    if publisher.poll() is not None or bridge.poll() is not None:
        obs.script_log(obs.LOG_ERROR, f"发布进程已退出，请查看 {log_path}")
        end(None, None)
        return
    if phase in ("arming", "starting") and time.monotonic() >= deadline:
        obs.script_log(obs.LOG_ERROR, "等待 OBS 开播超时，请查看直播日志")
        end(None, None)
        return
    if phase == "arming":
        # A TCP connect probe would consume the listener's single input.
        # 通过探测端口占用等待监听就绪，避免连接探测抢占唯一输入。
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", local_port))
            except OSError:
                try:
                    if obs.obs_frontend_streaming_active():
                        raise RuntimeError("OBS 已启动其他直播")
                    start_stream()
                except (AttributeError, RuntimeError, TypeError) as error:
                    obs.script_log(obs.LOG_ERROR, str(error))
                    end(None, None)


def end(props, prop):
    global phase
    if phase in ("starting", "live"):
        phase = "stopping"
        obs.obs_frontend_streaming_stop()
        if phase == "stopping" and not obs.obs_frontend_streaming_active():
            stop()
    elif phase != "stopping":
        stop()
    return False


def stop():
    global publisher, bridge, session_dir, previous_service, phase
    phase = "idle"
    for child in (bridge, publisher):
        if child and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
    publisher = bridge = None
    if session_dir:
        session_dir.cleanup()
        session_dir = None
    if previous_service:
        obs.obs_frontend_set_streaming_service(previous_service)
        obs.obs_service_release(previous_service)
        previous_service = None


def event(kind):
    global phase
    if phase == "starting" and kind == obs.OBS_FRONTEND_EVENT_STREAMING_STARTED:
        phase = "live"
    elif phase in ("starting", "live", "stopping") and kind == obs.OBS_FRONTEND_EVENT_STREAMING_STOPPED:
        stop()


def script_load(settings):
    script_update(settings)
    obs.obs_frontend_add_event_callback(event)
    obs.timer_add(poll, 200)


def script_unload():
    obs.timer_remove(poll)
    end(None, None)
    stop()
