"""Pinned Monocypher sources shared by the STM32 and host builds."""
from hashlib import sha512
from io import BytesIO
from pathlib import Path
import tarfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
VERSION = "4.0.3"
ARCHIVE = f"monocypher-{VERSION}.tar.gz"
SHA512 = (
    "40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251e"
    "c0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965"
)
FILES = ("monocypher.c", "monocypher.h", "optional/monocypher-ed25519.c", "optional/monocypher-ed25519.h")


def monocypher_sources():
    cache = ROOT / ".pio/stm32-ota-deps"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ARCHIVE
    if archive.is_file():
        data = archive.read_bytes()
    else:
        with urlopen(f"https://monocypher.org/download/{ARCHIVE}", timeout=30) as response:
            data = response.read(5_000_000)
    if sha512(data).hexdigest() != SHA512:
        raise ValueError(f"Monocypher checksum mismatch for {archive}")
    if not archive.is_file():
        archive.write_bytes(data)
    sources = cache / f"monocypher-{VERSION}-src"
    with tarfile.open(fileobj=BytesIO(data), mode="r:gz") as release:
        for name in FILES:
            destination = sources / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            contents = release.extractfile(f"monocypher-{VERSION}/src/{name}").read()
            if not destination.is_file() or destination.read_bytes() != contents:
                destination.write_bytes(contents)
    return sources
