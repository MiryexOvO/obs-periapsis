"""Start an OBS program broadcast through PERI. / 一键发布 OBS 节目画面与混音。"""
import os
import runpy
import ctypes
import msvcrt
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
native = None
session_dir = None
previous_service = None
phase = "idle"
deadline = 0.0
log_path = None


def load_native():
    global native
    if native is not None:
        return native
    path = plugin_dir / "native/peri-output.dll"
    if not path.is_file():
        raise ValueError(f"缺少 PERI 原生输出模块：{path}")
    # Use libobs's module loader so it owns registration and lifetime across script reloads.
    # 通过 libobs 加载模块，脚本重载时沿用已注册的类型和模块生命周期。
    api = ctypes.CDLL("obs.dll")
    ptr = ctypes.c_void_p
    api.obs_get_module.argtypes = [ctypes.c_char_p]
    api.obs_get_module.restype = ptr
    module = api.obs_get_module(b"peri-output")
    if not module:
        module = ptr()
        api.obs_open_module.argtypes = [ctypes.POINTER(ptr), ctypes.c_char_p, ctypes.c_char_p]
        api.obs_open_module.restype = ctypes.c_int
        api.obs_init_module.argtypes = [ptr]
        api.obs_init_module.restype = ctypes.c_bool
        if api.obs_open_module(ctypes.byref(module), str(path).encode(), str(path.parent).encode()) != 0 or not api.obs_init_module(module):
            raise ValueError("无法加载 PERI 原生模块，请查看 OBS 日志并确认使用 OBS 32.1.x x64")
    api.obs_get_module_binary_path.argtypes = [ptr]
    api.obs_get_module_binary_path.restype = ctypes.c_char_p
    loaded = api.obs_get_module_binary_path(module)
    dll = ctypes.CDLL(os.fsdecode(loaded))
    dll.peri_bind_sink.argtypes = [ptr]
    dll.peri_bind_sink.restype = ctypes.c_bool
    native = dll
    return dll


def script_description():
    return "填写服务器和推流码，点击开始即可直播。首次连接请在高级设置选择服务器证书。"


def script_defaults(settings):
    for key, value in (("stream_key", "stream"), ("name", "localhost"),
                       ("resource", "live/av1"), ("core", str(plugin_dir.parent / "periapsis")), ("cert", str(plugin_dir.parent / "periapsis/cert.der"))):
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
                       ("resource", "资源名"),):
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
    for key in ("server", "stream_key", "link", "core", "name", "resource", "cert"):
        config[key] = obs.obs_data_get_string(settings, key)


def begin(props, prop):
    global publisher, session_dir, phase, deadline, log_path
    if phase != "idle":
        return False
    if obs.obs_frontend_streaming_active():
        obs.script_log(obs.LOG_WARNING, "OBS 已有直播，请先结束当前直播。")
        return False
    try:
        root = Path(config["core"]).resolve()
        name, resource, cert = (config[k] for k in ("name", "resource", "cert"))
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
        env = dict(os.environ, PERI_PUBLISH_TOKEN=token)
        dll = load_native()
        binaries = [root / "target" / profile / "peri.exe" for profile in ("release", "debug")]
        binary = max((p for p in binaries if p.is_file()), key=lambda p: p.stat().st_mtime, default=None)
        if binary is None:
            raise ValueError("找不到 peri.exe，请先构建 peri-native")
        args = [str(binary), "av-publish", address, name, cert, resource]
        log_path = root / "dist/obs-publish.log"
        log_path.parent.mkdir(exist_ok=True)
        with log_path.open("wb") as output:
            publisher = subprocess.Popen(args, cwd=root, env=env, stdout=output,
                                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            if not dll.peri_bind_sink(msvcrt.get_osfhandle(publisher.stdin.fileno())):
                raise OSError("无法绑定 PERI 编码包管道")
        finally:
            publisher.stdin.close()
        start_stream()
    except (OSError, ValueError, KeyError, RuntimeError, AttributeError) as error:
        stop()
        obs.script_log(obs.LOG_ERROR, str(error))
    return False


def start_stream():
    global previous_service, phase, deadline
    settings = obs.obs_data_create()
    try:
        service = obs.obs_service_create("peri_native_service", "PERI", settings, None)
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
    global phase, deadline
    if phase == "idle":
        return
    if phase in ("stopping", "finishing"):
        # Clean up on the timer, outside OBS output callbacks.
        # 在定时器中清理，避免在 OBS 输出回调内等待或切换服务。
        if phase == "stopping" and time.monotonic() >= deadline:
            if obs.obs_frontend_streaming_active():
                output = obs.obs_frontend_get_streaming_output()
                if output:
                    try:
                        obs.obs_output_force_stop(output)
                    finally:
                        obs.obs_output_release(output)
                deadline = time.monotonic() + 3
            else:
                phase = "finishing"
        if phase == "finishing" and (publisher is None or publisher.poll() is not None):
            stop()
        return
    if publisher.poll() is not None:
        obs.script_log(obs.LOG_ERROR, f"发布进程已退出，请查看 {log_path}")
        end(None, None)
        return
    if phase == "starting" and time.monotonic() >= deadline:
        obs.script_log(obs.LOG_ERROR, "等待 OBS 开播超时，请查看直播日志")
        end(None, None)
        return


def begin_shutdown():
    global phase, deadline
    if phase in ("idle", "stopping", "finishing"):
        return
    phase, deadline = "stopping", time.monotonic() + 3
    # Close the pipe reader before waiting for OBS. / 先关闭管道读端，再等待 OBS 停播。
    if publisher and publisher.poll() is None:
        publisher.terminate()


def end(props, prop):
    if phase in ("starting", "live"):
        begin_shutdown()
        obs.obs_frontend_streaming_stop()
    return False


def stop():
    global publisher, session_dir, previous_service, phase
    phase = "idle"
    if publisher and publisher.poll() is None:
        publisher.terminate()
        try:
            publisher.wait(timeout=2)
        except subprocess.TimeoutExpired:
            publisher.kill()
            publisher.wait()
    publisher = None
    if native:
        native.peri_bind_sink(None)
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
    elif phase in ("starting", "live") and kind == obs.OBS_FRONTEND_EVENT_STREAMING_STOPPING:
        begin_shutdown()
    elif phase in ("starting", "live", "stopping") and kind == obs.OBS_FRONTEND_EVENT_STREAMING_STOPPED:
        begin_shutdown()
        phase = "finishing"


def script_load(settings):
    script_update(settings)
    obs.obs_frontend_add_event_callback(event)
    obs.timer_add(poll, 200)


def script_unload():
    obs.timer_remove(poll)
    end(None, None)
    stop()
