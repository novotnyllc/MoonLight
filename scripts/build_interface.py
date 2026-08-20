#   ESP32 SvelteKit --
#
#   A simple, secure and extensible framework for IoT projects for ESP32 platforms
#   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
#   https://github.com/theelims/ESP32-sveltekit
#
#   Copyright (C) 2018 - 2023 rjwats
#   Copyright (C) 2023 - 2025 theelims
#   Copyright (C) 2023 Maxtrium B.V. [ code available under dual license ]
#   Copyright (C) 2024 runeharlyk
#   Copyright (C) 2025 hmbacher
#
#   All Rights Reserved. This software may be modified and distributed under
#   the terms of the LGPL v3 license. See the LICENSE file for details.

from pathlib import Path
from shutil import copytree, rmtree
from os.path import exists, getmtime
import os
import sys
import gzip
import mimetypes
from datetime import datetime

# Already-compressed or binary assets: embed raw bytes (no Content-Encoding).
SKIP_COMPRESS_SUFFIXES = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico", ".woff", ".woff2",
    ".mp3", ".mp4", ".zip", ".gz", ".br",
}

# Import shared prebuild utilities
from prebuild_utils import is_build_task

# Check if this script should run
if not is_build_task(['build', 'upload', 'buildfs', 'erase_upload']):
    # Skip script execution for all other tasks
    print("Skipping interface build for non-build task.")
    sys.exit(0)

Import("env")

project_dir = env["PROJECT_DIR"]
buildFlags = env.ParseFlags(env["BUILD_FLAGS"])

interface_dir = project_dir + "/interface"
output_file = project_dir + "/lib/framework/WWWData.h"
source_www_dir = interface_dir + "/src"
build_dir = interface_dir + "/build"
filesystem_dir = project_dir + "/data/www"


def find_latest_timestamp_for_app():
    inputs = [Path(project_dir, "scripts", "build_interface.py"), *Path(source_www_dir).rglob("*"), *Path(interface_dir).glob("*.config.*")]
    inputs += list(Path(interface_dir).glob("package*.json"))
    inputs += list(Path(interface_dir, "static").rglob("*"))
    return max(path.stat().st_mtime for path in inputs if path.is_file())


def should_regenerate_output_file():
    if not flag_exists("EMBED_WWW") or not exists(output_file):
        return True
    last_source_change = find_latest_timestamp_for_app()
    last_build = getmtime(output_file)

    print(
        f"Newest file: {datetime.fromtimestamp(last_source_change)}, output file: {datetime.fromtimestamp(last_build)}"
    )

    return last_build < last_source_change


def gzip_file(file):
    raw = Path(file).read_bytes()
    Path(file + ".gz").write_bytes(gzip.compress(raw, compresslevel=9, mtime=0))
    Path(file).unlink()


def should_compress_asset(asset_path: str) -> bool:
    suffix = Path(asset_path).suffix.lower()
    return suffix not in SKIP_COMPRESS_SUFFIXES


def compress_asset_bytes(asset_path: str, raw: bytes) -> tuple[bytes, str]:
    if not should_compress_asset(asset_path):
        return raw, ""
    # ponytail: gzip not brotli — ESP embeds one encoding; br breaks clients without br decode
    return gzip.compress(raw, compresslevel=9, mtime=0), "gzip"


def flag_exists(flag):
    for define in buildFlags.get("CPPDEFINES"):
        if (define == flag or (isinstance(define, list) and define[0] == flag)):
            return True
    return False


def get_package_manager():
    if exists(os.path.join(interface_dir, "pnpm-lock.yaml")):
        return "pnpm"
    if exists(os.path.join(interface_dir, "yarn.lock")):
        return "yarn"
    else:
        return "npm"


def build_webapp():
    package_manager = get_package_manager()
    print(f"Building interface with {package_manager}")
    os.chdir(interface_dir)
    env.Execute(f"{package_manager} install")
    env.Execute(f"{package_manager} run build")
    os.chdir("..")


def embed_webapp():
    if flag_exists("EMBED_WWW"):
        print("Converting interface to PROGMEM")
        build_progmem()
        return
    add_app_to_filesystem()


def build_progmem():
    mimetypes.init()
    with open(output_file, "w") as progmem:
        progmem.write("#include <functional>\n")
        progmem.write("#include <Arduino.h>\n")

        assetMap = {}

        assets = sorted(
            Path(build_dir).rglob("*.*"),
            key=lambda path: path.relative_to(build_dir).as_posix(),
        )
        for idx, path in enumerate(assets):
            asset_path = path.relative_to(build_dir).as_posix()
            asset_mime = (
                mimetypes.guess_type(asset_path)[0] or "application/octet-stream"
            )
            print(f"Converting {asset_path}")

            asset_var = f"ESP_SVELTEKIT_DATA_{idx}"
            progmem.write(f"// {asset_path}\n")
            progmem.write(f"const uint8_t {asset_var}[] = {{\n\t")
            raw = path.read_bytes()
            file_data, content_encoding = compress_asset_bytes(asset_path, raw)
            if content_encoding:
                print(f"  gzip: {len(raw)} -> {len(file_data)} bytes")
            else:
                print(f"  raw: {len(file_data)} bytes")

            for i, byte in enumerate(file_data):
                if i and not (i % 16):
                    progmem.write("\n\t")
                progmem.write(f"0x{byte:02X},")

            progmem.write("\n};\n\n")
            assetMap[asset_path] = {
                "name": asset_var,
                "mime": asset_mime,
                "size": len(file_data),
                "encoding": content_encoding,
            }

        progmem.write(
            "typedef std::function<void(const String& uri, const String& contentType, const uint8_t * content, size_t len, const char* contentEncoding)> RouteRegistrationHandler;\n\n"
        )
        progmem.write("class WWWData {\n")
        progmem.write("\tpublic:\n")
        progmem.write(
            "\t\tstatic void registerRoutes(RouteRegistrationHandler handler) {\n"
        )

        for asset_path, asset in assetMap.items():
            encoding = asset["encoding"].replace('"', '\\"')
            progmem.write(
                f'\t\t\thandler("/{asset_path}", "{asset["mime"]}", {asset["name"]}, {asset["size"]}, "{encoding}");\n'
            )

        progmem.write("\t\t}\n")
        progmem.write("};\n\n")


def add_app_to_filesystem():
    build_path = Path(build_dir)
    www_path = Path(filesystem_dir)
    if www_path.exists() and www_path.is_dir():
        rmtree(www_path)
    print("Copying and compress interface to data directory")
    copytree(build_path, www_path)
    for current_path, directories, files in os.walk(www_path):
        directories.sort()
        for file in sorted(files):
            gzip_file(os.path.join(current_path, file))
    if ("upload" in BUILD_TARGETS):
        print("Build LittleFS file system image and upload to ESP32")
        env.Execute("pio run --target uploadfs")


print("running: build_interface.py")
if should_regenerate_output_file():
    build_webapp()
    embed_webapp()
