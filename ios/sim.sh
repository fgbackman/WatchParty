#!/bin/sh
# ios/sim.sh APP BUNDLE_ID SHOT — install the built app on an iPhone simulator,
# launch it and save a screenshot. Picks a booted iPhone, else the newest one.
app="$1"; bid="$2"; shot="$3"
udid=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
devs = [x for r, l in d.items() if "iOS" in r for x in l if x["name"].startswith("iPhone")]
booted = [x for x in devs if x["state"] == "Booted"]
pick = (booted or devs[-1:] or [{"udid": "", "name": ""}])[0]
print(pick["udid"])
sys.stderr.write("simulator: %s\n" % pick["name"])')
[ -n "$udid" ] || { echo "No iPhone simulator found (Xcode > Settings > Components)."; exit 1; }
xcrun simctl boot "$udid" 2>/dev/null
xcrun simctl bootstatus "$udid" -b >/dev/null || exit 1
open -a Simulator --args -CurrentDeviceUDID "$udid" 2>/dev/null
xcrun simctl terminate "$udid" "$bid" 2>/dev/null
xcrun simctl install "$udid" "$app" || exit 1
xcrun simctl launch "$udid" "$bid" || exit 1
sleep "${WAIT:-10}"
xcrun simctl io "$udid" screenshot "$shot" && echo "screenshot: $shot"
