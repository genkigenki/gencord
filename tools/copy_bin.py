# PlatformIO post-build step: copy the app image to ./gencord.bin
# (the file to install from the M5Launcher: SD card or WebUI OTA).
import shutil
Import("env")  # noqa: F821


def copy_bin(source, target, env):
    shutil.copyfile(str(target[0]), env.subst("$PROJECT_DIR/gencord.bin"))
    print("gencord.bin updated")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_bin)  # noqa: F821
