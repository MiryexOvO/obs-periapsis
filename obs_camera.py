"""OBS Virtual Camera -> PERI AV1 worker (Windows)."""
import queue
import threading

import av


def camera_frames():
    latest = queue.Queue(maxsize=1)

    def capture():
        def replace(value):
            try:
                latest.get_nowait()
            except queue.Empty:
                pass
            latest.put_nowait(value)

        try:
            with av.open("video=OBS Virtual Camera", format="dshow") as device:
                for frame in device.decode(video=0):
                    replace(frame)
            raise RuntimeError("OBS Virtual Camera stopped")
        except Exception as error:
            replace(error)

    # 只保留最新画面，避免不同帧率造成积压。 / Drop stale frames across clock rates.
    threading.Thread(target=capture, daemon=True).start()
    while True:
        frame = latest.get()
        if isinstance(frame, Exception):
            raise frame
        yield frame


if __name__ == "__main__":
    import os
    import runpy

    encoder = runpy.run_path(os.environ["PERI_ENCODER_SCRIPT"])
    encoder["serve"](camera_frames())
