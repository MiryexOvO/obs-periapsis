"""Publish the OBS virtual camera through the local PERI client on Windows."""
import os
import subprocess
from pathlib import Path

import obspython as obs

plugin_dir = Path(__file__).resolve().parent
config = {}
publisher = None


def script_description():
    return "PERI 视频发布。配置完成后，使用 OBS 的「启动虚拟摄像机」开始推流。"


def script_defaults(settings):
    for key, value in (("address", "127.0.0.1:4444"), ("name", "localhost"),
                       ("resource", "live/av1"), ("core", str(plugin_dir.parent / "periapsis")), ("cert", str(plugin_dir.parent / "periapsis/cert.der")),
                       ("python", os.environ.get("PERI_PYTHON", "python"))):
        obs.obs_data_set_default_string(settings, key, value)


def script_properties():
    props = obs.obs_properties_create()
    for key, label in (("address", "Relay IP:端口"), ("name", "证书服务器名"),
                       ("resource", "资源名"), ("python", "编码用 Python 路径")):
        obs.obs_properties_add_text(props, key, label, obs.OBS_TEXT_DEFAULT)
    obs.obs_properties_add_path(props, "core", "PERI 项目目录", obs.OBS_PATH_DIRECTORY, None, None)
    for key, label in (("cert", "服务器证书 (.der)"), ("token", "发布令牌文件")):
        obs.obs_properties_add_path(props, key, label, obs.OBS_PATH_FILE, "*.*", None)
    obs.obs_properties_add_bool(props, "enabled", "启动虚拟摄像机时发布到 PERI")
    return props


def script_update(settings):
    for key in ("core", "address", "name", "resource", "python", "cert", "token"):
        config[key] = obs.obs_data_get_string(settings, key)
    config["enabled"] = obs.obs_data_get_bool(settings, "enabled")
    if not config["enabled"]:
        stop()


def start():
    global publisher
    if not config.get("enabled") or publisher is not None:
        return
    try:
        root = Path(config["core"]).resolve()
        token = Path(config["token"]).read_text(encoding="utf-8-sig").strip()
        if not (32 <= len(token) <= 128 and all("!" <= c <= "~" for c in token)):
            raise ValueError("发布令牌文件格式无效")
        env = dict(os.environ, PERI_PUBLISH_TOKEN=token, PERI_ENCODER_SCRIPT=str(root / "scripts/live-av1-worker.py"),
                   PERI_PYTHON=config["python"], PYTHONPATH=str(root / "dist/live-python"))
        args = [str(root / "target/debug/peri.exe"), "live", config["address"], config["name"],
                config["cert"], config["resource"], str(plugin_dir / "obs_camera.py"), "13500"]
        # 只保存日志路径和进程句柄。 / Keep credentials out of OBS settings and logs.
        log = root / "dist/obs-publish.log"
        with log.open("wb") as output:
            publisher = subprocess.Popen(args, cwd=root, env=env, stdout=output,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
        obs.script_log(obs.LOG_INFO, "PERI 发布进程已启动：" + str(log))
    except (OSError, ValueError) as error:
        obs.script_log(obs.LOG_ERROR, str(error))


def poll():
    global publisher
    if publisher is not None and publisher.poll() is not None:
        obs.script_log(obs.LOG_INFO if publisher.returncode == 0 else obs.LOG_ERROR,
                       f"PERI 已退出 ({publisher.returncode})，详见 dist/obs-publish.log")
        publisher = None


def stop():
    global publisher
    if publisher is not None:
        # 关闭发布进程也会关闭编码器请求管道。 / Closing the publisher releases worker stdin.
        publisher.terminate()
        publisher.wait(timeout=3)
        publisher = None


def event(kind):
    if kind == obs.OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
        start()
    elif kind == obs.OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
        stop()


def script_load(settings):
    obs.obs_frontend_add_event_callback(event)
    obs.timer_add(poll, 1000)


def script_unload():
    obs.timer_remove(poll)
    stop()
