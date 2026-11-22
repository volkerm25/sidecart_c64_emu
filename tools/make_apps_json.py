#!/usr/bin/env python3
"""Write the store's apps.json for this app.

  make_apps_json.py <descriptor> <previous apps.json> <output>

<descriptor> is the dist/<uuid>.json build.sh writes. <previous apps.json>
is the list the last release published (a missing or empty file means
none); its current version of this app, and its older ones, become
previous_versions. The output holds this app only, in the shape
sidecartridge-microfirmwares-store's schema/apps.schema.json requires.
"""

import json
import os
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    descriptor, previous_path, output = sys.argv[1:]
    with open(descriptor) as f:
        app = json.load(f)

    previous = []
    if os.path.exists(previous_path) and os.path.getsize(previous_path) > 0:
        with open(previous_path) as f:
            for old in json.load(f).get("apps", []):
                if old.get("uuid") != app["uuid"]:
                    continue
                previous.append({k: old[k] for k in ("version", "binary", "md5")})
                previous.extend(old.get("previous_versions", []))

    seen = {app["version"]}
    app["previous_versions"] = []
    for entry in previous:
        if entry["version"] not in seen:
            seen.add(entry["version"])
            app["previous_versions"].append(entry)

    with open(output, "w") as f:
        json.dump({"apps": [app]}, f, indent=2)
        f.write("\n")
    print("%s: %s %s, %d previous" % (output, app["name"], app["version"],
                                      len(app["previous_versions"])))


if __name__ == "__main__":
    main()
