import subprocess
import datetime
import os
import re
import tempfile

Import("env")

DEFAULT_GITHUB_REPOSITORY = "David-Huynh/gaggiuino-gaggimate"
DEFAULT_VERSION_PREFIX = "v1.8"


def sanitize_github_repository(repository):
    repository = repository.strip().strip("/")
    parts = repository.split("/")
    if len(parts) < 2:
        return ""

    owner = parts[0]
    repo = parts[1]
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
    if not owner or not repo:
        return ""
    if any(ch not in allowed for ch in owner + repo):
        return ""
    return owner + "/" + repo


def repository_from_remote(remote_url):
    remote_url = remote_url.strip()
    if remote_url.endswith(".git"):
        remote_url = remote_url[:-4]

    prefixes = [
        "git@github.com:",
        "ssh://git@github.com/",
        "https://github.com/",
        "http://github.com/",
    ]
    for prefix in prefixes:
        if remote_url.startswith(prefix):
            return sanitize_github_repository(remote_url[len(prefix):])
    return ""


def get_firmware_specifier_build_flag():
    explicit_version = os.environ.get("GAGGIMATE_BUILD_VERSION", "")
    if explicit_version:
        if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.+-]+)?", explicit_version):
            raise ValueError("GAGGIMATE_BUILD_VERSION must be a firmware version such as v1.9.0")
        print("Build version: " + explicit_version)
        return '#define BUILD_GIT_VERSION "' + explicit_version + '"'
    ret = subprocess.run(
        ["git", "describe", "--tags", "--dirty", "--match", "v*"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    ) #Uses any tags
    build_version = ret.stdout.strip()
    if not build_version:
        count_ret = subprocess.run(["git", "rev-list", "--count", "HEAD"], stdout=subprocess.PIPE, text=True)
        run_number = count_ret.stdout.strip()
        if not run_number.isdigit():
            run_number = "0"
        build_version = DEFAULT_VERSION_PREFIX + ".0-dev." + run_number
    build_flag = "#define BUILD_GIT_VERSION \"" + build_version + "\""
    print ("Build version: " + build_version)
    return build_flag


def get_repository_specifier_build_flag():
    repository = sanitize_github_repository(os.environ.get("GITHUB_REPOSITORY", ""))
    if not repository:
        ret = subprocess.run(["git", "config", "--get", "remote.origin.url"], stdout=subprocess.PIPE, text=True)
        repository = repository_from_remote(ret.stdout)
    if not repository:
        repository = DEFAULT_GITHUB_REPOSITORY

    build_flag = "#define BUILD_GIT_REPOSITORY \"" + repository + "\""
    print ("Build repository: " + repository)
    return build_flag


def get_time_specifier_build_flag():
    build_timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    build_flag = "#define BUILD_TIMESTAMP \"" + build_timestamp + "\""
    print ("Build date: " + build_timestamp)
    return build_flag

content = (
        '#pragma once\n' +
        '#ifndef GIT_VERSION_H\n' +
        '#define GIT_VERSION_H\n' +
        get_firmware_specifier_build_flag() + '\n' +
        get_repository_specifier_build_flag() + '\n' +
        get_time_specifier_build_flag() + '\n'
        '#endif\n'
)

# Write to a unique temp file and swap it in atomically, so a build running in
# parallel in the same workspace never reads a truncated version.h.
fd, tmp_path = tempfile.mkstemp(dir='src', prefix='version.h.', suffix='.tmp')
try:
    with os.fdopen(fd, 'w') as f:
        f.write(content)
    os.replace(tmp_path, 'src/version.h')
except BaseException:
    os.unlink(tmp_path)
    raise
