Import("env")

# Sync layout editor + legacy web dist into firmware/data before build/uploadfs.
import os
import shutil

data_dir = os.path.join(env["PROJECT_DIR"], "data")
web_dist = os.path.join(env["PROJECT_DIR"], "..", "web", "dist")
editor_src = os.path.join(env["PROJECT_DIR"], "..", "tools", "web-editor")
editor_dst = os.path.join(data_dir, "editor")


def _sync_editor():
    if not os.path.isdir(editor_src):
        return
    os.makedirs(editor_dst, exist_ok=True)
    for name in ("index.html", "app.js", "style.css"):
        src = os.path.join(editor_src, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(editor_dst, name))


def _sync_data_sources(target, source, env):
    os.makedirs(data_dir, exist_ok=True)
    _sync_editor()
    if os.path.isdir(web_dist):
        for x in os.listdir(data_dir):
            if x == "editor":
                continue
            p = os.path.join(data_dir, x)
            if os.path.isdir(p):
                shutil.rmtree(p, True)
            else:
                os.remove(p)
        for x in os.listdir(web_dist):
            sp = os.path.join(web_dist, x)
            dp = os.path.join(data_dir, x)
            if os.path.isdir(sp):
                shutil.copytree(sp, dp, dirs_exist_ok=True)
            else:
                shutil.copy2(sp, dp)


# Editor on omote.local is optional (~118 KB). Only copy into LittleFS when you run uploadfs.
if os.path.isdir(editor_src) or os.path.isdir(web_dist):
    env.AddPreAction(
        "uploadfs",
        env.Action(_sync_data_sources, "Sync web assets before uploadfs"),
    )
