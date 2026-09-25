#!/usr/bin/env python3
"""Publish a devOS firmware build for over-the-air update.

Copies the app image into an output folder next to a devos-manifest.json
(version from components/devos_config/include/devos_config.h, size, sha256).
Serve that folder over HTTP and point Settings > System > Feed at the
manifest URL:

    idf.py build
    tools/make_ota_manifest.py --out /srv/devos --notes "Tailscale client"
    python3 -m http.server 8090 --directory /srv/devos
    # Feed URL: http://<this machine>:8090/devos-manifest.json

Only versions newer than the one running on the Tab5 are offered, so bump
DEVOS_VERSION_MAJOR/MINOR/PATCH in devos_config.h before publishing.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_version():
    path = os.path.join(ROOT, "components/devos_config/include/devos_config.h")
    text = open(path, encoding="utf-8").read()
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"#define\s+DEVOS_VERSION_%s\s+(\d+)" % name, text)
        if not m:
            sys.exit("could not read DEVOS_VERSION_%s from %s" % (name, path))
        parts.append(m.group(1))
    return ".".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", help="app image (default: build_full/ or build/ tab5-devos.bin)")
    ap.add_argument("--out", default=os.path.join(ROOT, "ota_out"), help="output folder (default: ota_out/)")
    ap.add_argument("--version", help="override the version (default: from devos_config.h)")
    ap.add_argument("--notes", default="", help="short release notes shown on the device")
    ap.add_argument("--base-url", default="", help="absolute URL prefix for the image (default: relative)")
    args = ap.parse_args()

    image = args.bin
    if not image:
        for cand in ("build_full/tab5-devos.bin", "build/tab5-devos.bin"):
            if os.path.exists(os.path.join(ROOT, cand)):
                image = os.path.join(ROOT, cand)
                break
    if not image or not os.path.exists(image):
        sys.exit("no firmware image found; build first or pass --bin")

    data = open(image, "rb").read()
    if not data or data[0] != 0xE9:
        sys.exit("%s does not look like an ESP app image" % image)
    version = args.version or read_version()
    name = "tab5-devos-%s.bin" % version

    os.makedirs(args.out, exist_ok=True)
    shutil.copyfile(image, os.path.join(args.out, name))
    manifest = {
        "version": version,
        "url": (args.base_url.rstrip("/") + "/" + name) if args.base_url else name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "notes": args.notes[:190],
    }
    with open(os.path.join(args.out, "devos-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("published %s (%d KB) -> %s" % (version, len(data) // 1024, args.out))
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
