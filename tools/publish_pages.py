#!/usr/bin/env python3
"""Build the public GitHub Pages site for devOS: a browser flasher + the OTA feed.

The output folder holds only what installing and updating the firmware needs:

    index.html                  "Install devOS" page (ESP Web Tools, Chrome / Edge), with a
                                version picker
    firmware/<version>/         each release offered: ESP Web Tools manifest.json, release.json
                                (date, notes) and the bootloader / partitions / otadata / app
    firmware/versions.json      the releases offered, newest first
    firmware/manifest.json      the latest release (old links)
    ota/devos-manifest.json     OTA feed the Tab5 checks (Settings > System)
    ota/tab5-devos-<ver>.bin
    .nojekyll

Releases already in <out>/firmware stay on the page; --add-old adds an
older release's app image (e.g. from its GitHub release). Nothing else from
this repository is copied. Before writing, every file is
scanned for identifying strings (home paths, e-mail addresses, and the
strings in ~/.config/devos/publish-deny.txt or --deny); a hit aborts the publish.

    idf.py build
    tools/publish_pages.py --build build --out docs --notes "ADS-B fixes"
    # then commit + push; GitHub Pages serves docs/

The Tab5's default OTA feed (components/devos_ota/devos_ota.c) must point at
<pages url>/ota/devos-manifest.json. Bump DEVOS_VERSION_* in devos_config.h
before each release. The OTA manifest also carries the image's build id, so a
Tab5 knows whether it runs exactly this image (and a rebuilt image of the same
version is still offered).
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
from make_ota_manifest import image_info, read_version  # noqa: E402

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


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def write_release(out, version, parts, app_src, notes, built):
    """firmware/<version>/: an ESP Web Tools manifest and every part it flashes.
    Older releases reuse this build's bootloader, partition table and otadata
    (the flash layout hasn't changed) with their own app image."""
    d = os.path.join(out, "firmware", version)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    manifest = {"name": "devOS", "version": version, "new_install_prompt_erase": True,
                "builds": [{"chipFamily": "ESP32-P4", "parts": []}]}
    for off, p in sorted(parts):
        name = os.path.basename(p)
        shutil.copyfile(app_src if name == "tab5-devos.bin" else p, os.path.join(d, name))
        manifest["builds"][0]["parts"].append({"path": name, "offset": off})
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(d, "release.json"), "w") as f:
        json.dump({"version": version, "built": built, "notes": notes[:190]}, f, indent=2)
        f.write("\n")


def releases(out):
    rel = []
    root = os.path.join(out, "firmware")
    for name in os.listdir(root):
        info = os.path.join(root, name, "release.json")
        if re.fullmatch(r"\d+\.\d+\.\d+", name) and os.path.exists(info):
            rel.append(json.load(open(info)))
    return sorted(rel, key=lambda r: vkey(r["version"]), reverse=True)


def page(rel):
    latest = rel[0]
    opts = "\n".join('    <option value="%s">%s%s</option>' % (html.escape(r["version"]), html.escape(r["version"]),
                                                              " (latest)" if r is latest else "")
                     for r in rel)
    data = json.dumps([{"v": r["version"], "n": r.get("notes", ""), "b": r.get("built", "")} for r in rel])
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
  li { margin: .3rem 0; } .notes { opacity: .8; min-height: 1.5em; }
  .pick { display: flex; gap: .6rem; align-items: center; margin: 1rem 0 .3rem; }
  select { font: inherit; padding: .3rem .5rem; border-radius: .4rem; }
</style>
</head>
<body>
<h1>devOS for M5Stack Tab5</h1>
<p class=ver>Latest version %(ver)s</p>
<div class=pick>
  <label for=ver>Version to install</label>
  <select id=ver>
%(opts)s
  </select>
</div>
<p class=notes id=notes></p>
<esp-web-install-button id=install manifest="firmware/%(ver)s/manifest.json">
  <button slot="activate">Install devOS</button>
  <span slot="unsupported">Your browser can't flash over USB. Use Chrome or Edge on a desktop.</span>
  <span slot="not-allowed">Flashing needs a secure (https) page.</span>
</esp-web-install-button>
<h2>Install</h2>
<ol>
  <li>Connect the Tab5 to this computer with a USB-C data cable.</li>
  <li>Pick a version (the latest, unless you are going back to an older one), click
      <b>Install devOS</b> and pick the Tab5's serial port (<i>USB JTAG/serial debug unit</i>).</li>
  <li>Choose <b>Erase device</b> for a first install. Settings, Wi-Fi and keys are
      kept in flash, so skip the erase when reinstalling or changing versions.</li>
  <li>When it finishes, the Tab5 restarts into devOS.</li>
</ol>
<p>If the port doesn't show up, hold the Tab5's BOOT button while you plug it in, then try again.</p>
<h2>Updates</h2>
<p>Once installed, the Tab5 updates itself over Wi-Fi to the latest version: <b>Settings &gt; System &gt;
Check for updates</b>. The feed is <code>ota/devos-manifest.json</code> on this site.</p>
<script>
  const R = %(data)s;
  const sel = document.getElementById("ver"), btn = document.getElementById("install"),
        notes = document.getElementById("notes");
  function pick() {
    const r = R[sel.selectedIndex], m = "firmware/" + r.v + "/manifest.json";
    btn.setAttribute("manifest", m);
    btn.manifest = m;
    notes.textContent = (r.n || "") + (r.b ? "  (built " + r.b + ")" : "");
  }
  sel.addEventListener("change", pick);
  pick();
</script>
</body>
</html>
""" % {"ewt": ESP_WEB_TOOLS, "ver": html.escape(latest["version"]), "opts": opts, "data": data}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.path.join(ROOT, "build"), help="IDF build folder (default: build/)")
    ap.add_argument("--out", required=True, help="Pages folder (docs/ in the public repo)")
    ap.add_argument("--notes", default="", help="short release notes (page + OTA)")
    ap.add_argument("--app", help="publish this app image instead of the build's (e.g. to redo the page "
                                  "without changing a released image)")
    ap.add_argument("--add-old", action="append", default=[], metavar="VERSION=APP.bin[=NOTES]",
                    help="also offer an older release's app image on the install page (repeatable)")
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
    if args.app:
        if (b"devOS v" + read_version().encode() + b"\0") not in open(args.app, "rb").read():
            sys.exit("%s is not devOS %s" % (args.app, read_version()))
        app = args.app

    old = []                                            # (version, app image, notes)
    for spec in args.add_old:
        bits = spec.split("=", 2)
        if len(bits) < 2 or not re.fullmatch(r"\d+\.\d+\.\d+", bits[0]) or not os.path.exists(bits[1]):
            sys.exit("--add-old wants VERSION=APP.bin[=NOTES], got %r" % spec)
        img = open(bits[1], "rb").read()
        if (b"devOS v" + bits[0].encode() + b"\0") not in img:
            sys.exit("%s is not devOS %s (its version string doesn't match)" % (bits[1], bits[0]))
        old.append((bits[0], bits[1], bits[2] if len(bits) > 2 else ""))

    hits = []
    for p in [p for _, p in parts] + [app] + [o[1] for o in old]:
        hits += scan(p, args.deny)
    if hits:
        sys.exit("identifying strings in the firmware, not publishing:\n  " + "\n  ".join(sorted(set(hits))))

    version = read_version()
    out = args.out
    fw = os.path.join(out, "firmware")
    os.makedirs(fw, exist_ok=True)
    for name in os.listdir(fw):                         # the old single-release layout
        if os.path.isfile(os.path.join(fw, name)):
            os.remove(os.path.join(fw, name))
    shutil.rmtree(os.path.join(out, "ota"), ignore_errors=True)
    os.makedirs(os.path.join(out, "ota"))

    data = open(app, "rb").read()
    build, built = image_info(data)
    write_release(out, version, parts, app, args.notes, built)
    for ver, img, notes in old:
        write_release(out, ver, parts, img, notes, image_info(open(img, "rb").read())[1])

    rel = releases(out)
    if rel[0]["version"] != version:
        sys.exit("firmware/ has %s, newer than this build (%s)" % (rel[0]["version"], version))
    with open(os.path.join(fw, "versions.json"), "w") as f:
        json.dump(rel, f, indent=2)
        f.write("\n")
    # the latest also at the old path, for links to firmware/manifest.json
    latest = json.load(open(os.path.join(fw, version, "manifest.json")))
    for part in latest["builds"][0]["parts"]:
        part["path"] = version + "/" + part["path"]
    with open(os.path.join(fw, "manifest.json"), "w") as f:
        json.dump(latest, f, indent=2)
        f.write("\n")

    ota_name = "tab5-devos-%s.bin" % version
    shutil.copyfile(app, os.path.join(out, "ota", ota_name))
    with open(os.path.join(out, "ota", "devos-manifest.json"), "w") as f:
        json.dump({"version": version, "url": ota_name, "size": len(data),
                   "sha256": hashlib.sha256(data).hexdigest(), "build": build, "notes": args.notes[:190]},
                  f, indent=2)
        f.write("\n")

    with open(os.path.join(out, "index.html"), "w") as f:
        f.write(page(rel))
    open(os.path.join(out, ".nojekyll"), "w").close()

    for dirpath, _, files in os.walk(out):              # final check over everything written
        if "/.git" in dirpath + "/":
            continue
        for fn in files:
            hits += scan(os.path.join(dirpath, fn), args.deny)
    if hits:
        sys.exit("identifying strings in %s:\n  %s" % (out, "\n  ".join(sorted(set(hits)))))
    print("published devOS %s, build %s (%s), %d KB app -> %s; install page offers %s" %
          (version, build[:8], built, len(data) // 1024, out, ", ".join(r["version"] for r in rel)))


if __name__ == "__main__":
    main()
