"""Publish the OBS virtual camera through the local PERI client on Windows."""
import os
import runpy
import subprocess
import tempfile
from pathlib import Path

import obspython as obs

plugin_dir = Path(__file__).resolve().parent
links = runpy.run_path(str(plugin_dir / "peri_link.py"))
parse_link = links["parse"]
config = {}
publisher = None
session_dir = None
pending = False
owns_camera = False


def script_description():
    return "填写服务器和推流码，首次在高级设置选择 Relay 的 cert.der。"


def script_defaults(settings):
    for key, value in (("stream_key", "stream"), ("address", "127.0.0.1:4444"), ("name", "localhost"),
                       ("resource", "live/av1"), ("core", str(plugin_dir.parent / "periapsis")), ("cert", str(plugin_dir.parent / "periapsis/cert.der")),
                       ("python", os.environ.get("PERI_PYTHON", "python"))):
        obs.obs_data_set_default_string(settings, key, value)


def script_properties():
    props = obs.obs_properties_create()
    obs.obs_properties_add_text(props, "server", "服务器（peri://IP）", obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_text(props, "stream_key", "推流码", obs.OBS_TEXT_PASSWORD)
    obs.obs_properties_add_bool(props, "program_av", "节目画面 + 混音（本机 WebM 输出）")
    obs.obs_properties_add_button(props, "start", "开始 PERI 推流", begin)
    obs.obs_properties_add_button(props, "stop", "停止 PERI 推流", end)
    advanced = obs.obs_properties_create()
    obs.obs_properties_add_text(advanced, "link", "旧版邀请链接", obs.OBS_TEXT_PASSWORD)
    for key, label in (("address", "Relay IP:端口"), ("name", "证书服务器名"),
                       ("resource", "资源名"), ("python", "编码用 Python 路径")):
        obs.obs_properties_add_text(advanced, key, label, obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_path(advanced, "core", "PERI 项目目录", obs.OBS_PATH_DIRECTORY, None, None)
    for key, label in (("cert", "服务器证书 (.der)"), ("token", "发布令牌文件")):
        obs.obs_properties_add_path(advanced, key, label, obs.OBS_PATH_FILE, "*.*", None)
    obs.obs_properties_add_bool(advanced, "enabled", "跟随虚拟摄像机自动推流")
    obs.obs_properties_add_group(props, "advanced", "本机路径和旧版配置", obs.OBS_GROUP_NORMAL, advanced)
    return props


def script_update(settings):
    was_enabled = config.get("enabled")
    program_av = obs.obs_data_get_bool(settings, "program_av")
    if config.get("program_av", False) != program_av:
        stop()
    config["program_av"] = program_av
    for key in ("server", "stream_key", "link", "core", "address", "name", "resource", "python", "cert", "token"):
        config[key] = obs.obs_data_get_string(settings, key)
    config["enabled"] = obs.obs_data_get_bool(settings, "enabled")
    if was_enabled and not config["enabled"]:
        stop()


def begin(props, prop):
    global pending, owns_camera
    if publisher is not None:
        return False
    try:
        if config["server"].strip():
            links["short_settings"](config["server"], config["stream_key"])
        else:
            parse_link(config["link"].strip())
    except ValueError as error:
        obs.script_log(obs.LOG_ERROR, str(error))
        return False
    if config.get("program_av") or obs.obs_frontend_virtualcam_active():
        start()
    else:
        pending = owns_camera = True
        obs.obs_frontend_start_virtualcam()
    return False


def end(props, prop):
    stop()
    return False


def start():
    global publisher, session_dir
    if publisher is not None:
        return
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
            token = Path(config["token"]).read_text(encoding="utf-8-sig").strip()
        if not (32 <= len(token) <= 128 and all("!" <= c <= "~" for c in token)):
            raise ValueError("发布令牌文件格式无效")
        env = dict(os.environ, PERI_PUBLISH_TOKEN=token, PERI_ENCODER_SCRIPT=str(root / "scripts/live-av1-worker.py"),
                   PERI_PYTHON=config["python"], PYTHONPATH=str(root / "dist/live-python"))
        args = [str(root / "target/debug/peri.exe"), "live", address, name,
                cert, resource, str(plugin_dir / "obs_camera.py"), "13500"]
        if config.get("program_av"):
            # OBS supplies the encoded program mix over loopback.
            # OBS 通过回环连接提供已编码的节目画面和混音。
            args = [config["python"], str(root / "scripts/program-av.py"), "publish",
                    address, name, cert, resource, "--peri", str(root / "target/debug/peri.exe")]
        # 令牌只通过环境传给发布进程。 / Pass the token to the child via its environment.
        log = root / "dist/obs-publish.log"
        with log.open("wb") as output:
            publisher = subprocess.Popen(args, cwd=root, env=env, stdout=output,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
        obs.script_log(obs.LOG_INFO, "PERI 发布进程已启动：" + str(log))
    except (OSError, ValueError) as error:
        stop()
        obs.script_log(obs.LOG_ERROR, str(error))


def poll():
    global publisher
    if publisher is not None and publisher.poll() is not None:
        obs.script_log(obs.LOG_INFO if publisher.returncode == 0 else obs.LOG_ERROR,
                       f"PERI 已退出 ({publisher.returncode})，详见 dist/obs-publish.log")
        publisher = None
        stop()


def stop():
    global publisher, session_dir, owns_camera, pending
    pending = False
    if publisher is not None:
        # 关闭发布进程也会关闭编码器请求管道。 / Closing the publisher releases worker stdin.
        publisher.terminate()
        publisher.wait(timeout=3)
        publisher = None
    if session_dir:
        session_dir.cleanup()
        session_dir = None
    if owns_camera:
        owns_camera = False
        if obs.obs_frontend_virtualcam_active():
            obs.obs_frontend_stop_virtualcam()


def event(kind):
    global pending
    if config.get("program_av"):
        return
    if kind == obs.OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
        if pending or config.get("enabled"):
            pending = False
            start()
    elif kind == obs.OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
        stop()


def script_load(settings):
    obs.obs_frontend_add_event_callback(event)
    obs.timer_add(poll, 1000)


def script_unload():
    obs.timer_remove(poll)
    stop()
