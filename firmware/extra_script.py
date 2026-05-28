Import("env")

# Build web assets into data/ before uploadfs when web/dist exists
import os
web_dist = os.path.join(env["PROJECT_DIR"], "..", "web", "dist")
data_dir = os.path.join(env["PROJECT_DIR"], "data")

if os.path.isdir(web_dist):
    env.AddPreAction(
        "uploadfs",
        env.VerboseAction(
            f'python -c "import shutil,os; d=\'{data_dir}\'; os.makedirs(d,exist_ok=True); '
            f"[shutil.rmtree(os.path.join(d,x),True) for x in os.listdir(d)]; "
            f"shutil.copytree(r'{web_dist}', d, dirs_exist_ok=True)\"",
            "Copying web/dist to firmware/data/",
        ),
    )
