#!/usr/bin/env python3
"""Build the public GitHub Pages site for devOS: a browser flasher + the OTA feed.

The output folder holds only what installing and updating the firmware needs:

    index.html                  "Install devOS" page (ESP Web Tools, Chrome / Edge)
    firmware/manifest.json      ESP Web Tools manifest (bootloader, partitions, app, otadata)
    firmware/*.bin
    ota/devos-manifest.json     OTA feed the Tab5 checks (Settings > System)
    ota/tab5-devos-<ver>.bin
    .nojekyll

Nothing else from this repository is copied. Before writing, every file is
scanned for identifying strings (home paths, e-mail addresses, and the
strings in ~/.config/devos/publish-deny.txt or --deny); a hit aborts the publish.

    idf.py build
    tools/publish_pages.py --build build --out docs --notes "ADS-B fixes"
    # then commit + push; GitHub Pages serves docs/

The Tab5's default OTA feed (components/devos_ota/devos_ota.c) must point at
<pages url>/ota/devos-manifest.json. Bump DEVOS_VERSION_* in devos_config.h
before each release: the Tab5 only offers newer versions.
"""
import argparse
import hashlib
import html
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from make_ota_manifest import read_version  # noqa: E402

ESP_WEB_TOOLS = "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"

# Generic identifying-string patterns. Your own names, networks and addresses go
# in DENY_FILE (one per line, outside the repo so they're never committed) or
# --deny; addresses the firmware shows as examples are fine to ship.
LEAK_PATTERNS = [
    (r"/home/[A-Za-z0-9_.-]+", "home directory path"),
    (r"/Users/[A-Za-z0-9_.-]+", "macOS home path"),
    (r"[A-Za-z]:\\Users\\", "Windows home path"),
    (r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z.]{2,}", "e-mail address"),
]
# e-mail-shaped protocol names and third-party copyright lines
LEAK_ALLOW_DOMAINS = ("openssh.com", "libssh2.org", "lysator.liu.se", "zx2c4.com", "e-szigno.hu",
                      "example.com", "example.org")
DENY_FILE = os.path.expanduser("~/.config/devos/publish-deny.txt")


def scan(path, deny):
    data = open(path, "rb").read()
    # printable runs, like `strings -n 6`
    text = "\n".join(m.decode("latin-1") for m in re.findall(rb"[\x20-\x7e]{6,}", data))
    hits = []
    for pat, why in LEAK_PATTERNS + [(re.escape(d), "denied string") for d in deny]:
        for m in re.finditer(pat, text, re.IGNORECASE):
            if not m.group(0).rstrip("W").lower().endswith(LEAK_ALLOW_DOMAINS):
                hits.append("%s: %s (%s)" % (os.path.basename(path), m.group(0), why))
    return hits


def page(version, notes):
    notes_html = "<p class=notes>%s</p>" % html.escape(notes) if notes else ""
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>devOS for M5Stack Tab5</title>
<script type="module" src="%(ewt)s"></script>
<style>
  :root { color-scheme: dark light; --acc: #00d1b2; }
  body { font: 16px/1.5 system-ui, sans-serif; max-width: 42rem; margin: 3rem auto; padding: 0 1.2rem; }
  h1 { margin-bottom: .2rem; } .ver { opacity: .7; margin-top: 0; }
  esp-web-install-button button { font: inherit; font-weight: 600; padding: .7rem 1.4rem;
    border: 0; border-radius: .5rem; background: var(--acc); color: #002; cursor: pointer; }
  code { background: rgba(127,127,127,.18); padding: .1rem .3rem; border-radius: .25rem; }
  li { margin: .3rem 0; } .notes { opacity: .8; }
</style>
</head>
<body>
<h1>devOS for M5Stack Tab5</h1>
<p class=ver>Version %(ver)s</p>
%(notes)s
<esp-web-install-button manifest="firmware/manifest.json">
  <button slot="activate">Install devOS</button>
  <span slot="unsupported">Your browser can't flash over USB. Use Chrome or Edge on a desktop.</span>
  <span slot="not-allowed">Flashing needs a secure (https) page.</span>
</esp-web-install-button>
<h2>Install</h2>
<ol>
  <li>Connect the Tab5 to this computer with a USB-C data cable.</li>
  <li>Click <b>Install devOS</b> and pick the Tab5's serial port
      (<i>USB JTAG/serial debug unit</i>).</li>
  <li>Choose <b>Erase device</b> for a first install. Settings, Wi-Fi and keys are
      kept in flash, so skip the erase when reinstalling.</li>
  <li>When it finishes, the Tab5 restarts into devOS.</li>
</ol>
<p>If the port doesn't show up, hold the Tab5's BOOT button while you plug it in, then try again.</p>
<h2>Updates</h2>
<p>Once installed, the Tab5 updates itself over Wi-Fi: <b>Settings &gt; System &gt; Check for updates</b>.
The feed is <code>ota/devos-manifest.json</code> on this site.</p>
</body>
</html>
""" % {"ewt": ESP_WEB_TOOLS, "ver": html.escape(version), "notes": notes_html}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"), help="IDF build folder (default: build/)")
    ap.add_argument("--out", required=True, help="Pages folder (docs/ in the public repo)")
    ap.add_argument("--notes", default="", help="short release notes (page + OTA)")
    ap.add_argument("--deny", action="append", default=[],
                    help="extra string that must not appear (repeatable; e.g. your name)")
    args = ap.parse_args()
    if os.path.exists(DENY_FILE):
        args.deny += [l.strip() for l in open(DENY_FILE) if l.strip() and not l.startswith("#")]

    b = args.build
    parts = []                                          # (offset, source path)
    fa = os.path.join(b, "flash_args")
    if not os.path.exists(fa):
        sys.exit("no %s; run idf.py build first" % fa)
    for line in open(fa):
        m = re.match(r"\s*(0x[0-9a-fA-F]+)\s+(\S+)", line)
        if m:
            parts.append((int(m.group(1), 16), os.path.join(b, m.group(2))))
    app = os.path.join(b, "tab5-devos.bin")
    if not any(p == app for _, p in parts):
        sys.exit("flash_args has no tab5-devos.bin")

    hits = []
    for _, p in parts:
        hits += scan(p, args.deny)
    if hits:
        sys.exit("identifying strings in the firmware, not publishing:\n  " + "\n  ".join(sorted(set(hits))))

    version = read_version()
    out = args.out
    os.makedirs(out, exist_ok=True)
    for sub in ("firmware", "ota"):                     # only the current release is kept
        shutil.rmtree(os.path.join(out, sub), ignore_errors=True)
        os.makedirs(os.path.join(out, sub))

    manifest = {"name": "devOS", "version": version, "new_install_prompt_erase": True,
                "builds": [{"chipFamily": "ESP32-P4", "parts": []}]}
    for off, p in sorted(parts):
        name = os.path.basename(p)
        shutil.copyfile(p, os.path.join(out, "firmware", name))
        manifest["builds"][0]["parts"].append({"path": name, "offset": off})
    with open(os.path.join(out, "firmware", "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    data = open(app, "rb").read()
    ota_name = "tab5-devos-%s.bin" % version
    shutil.copyfile(app, os.path.join(out, "ota", ota_name))
    with open(os.path.join(out, "ota", "devos-manifest.json"), "w") as f:
        json.dump({"version": version, "url": ota_name, "size": len(data),
                   "sha256": hashlib.sha256(data).hexdigest(), "notes": args.notes[:190]}, f, indent=2)
        f.write("\n")

    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(page(version, args.notes))
    open(os.path.join(out, ".nojekyll"), "w").close()

    for dirpath, _, files in os.walk(out):              # final check over everything written
        if "/.git" in dirpath + "/":
            continue
        for fn in files:
            hits += scan(os.path.join(dirpath, fn), args.deny)
    if hits:
        sys.exit("identifying strings in %s:\n  %s" % (out, "\n  ".join(sorted(set(hits)))))
    print("published devOS %s (%d KB app) -> %s" % (version, len(data) // 1024, out))


if __name__ == "__main__":
    main()
