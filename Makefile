# WatchParty — a C + Cocoa watch-together app for macOS.
#
#   make            build build/WatchParty.app
#   make run        build and (re)launch it      (touch .extra-instance first to open a 2nd copy)
#   make test       unit tests (JSON, utilities, player sync logic)
#   make selftest   end-to-end checks with no UI: the TLS-PSK transport, then "ask to join"
#                   (two processes); with an invite code in .robot-code it joins the LIVE room instead
#   make universal  build an arm64 + x86_64 binary
#   make ios-build  build the iPhone app for the simulator (needs Xcode)
#   make ios-sim    ...then install it on a simulator, launch it, and save build/ios-shot.png
#   make clean
#
# The dev loop only runs fixed targets; for anything else, put the target name in
# .task and request `test` — it runs that target instead.

APP      := WatchParty
BUILD    := build
BUNDLE   := $(BUILD)/$(APP).app
CONTENTS := $(BUNDLE)/Contents
BIN      := $(CONTENTS)/MacOS/$(APP)

CC       ?= clang
ARCHS    ?= -arch arm64 -arch x86_64
CFLAGS   ?= -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -fblocks -mmacosx-version-min=11.0 $(ARCHS)
LDFLAGS  ?= -framework Cocoa -framework WebKit -framework Network -framework Security \
            -framework CoreGraphics -lobjc $(ARCHS)

SRC      := src/main.c src/wp_app.c src/wp_room.c src/wp_net.c src/wp_lobby.c src/wp_ec.c src/wp_search.c \
            src/wp_search_parse.c src/wp_json.c src/wp_util.c src/wp_selftest.c src/wp_fancy.c
HDR      := src/objc_glue.h src/wp_app.h src/wp_room.h src/wp_net.h src/wp_lobby.h src/wp_search.h src/wp_json.h \
            src/wp_util.h src/wp_fancy.h src/wp_fancy_table.h
ICNS     := Resources/$(APP).icns

.PHONY: all run test selftest universal clean clean-bin icon ios-build ios-sim ios-project

all: $(BUNDLE)

$(BIN): $(SRC) $(HDR)
	@mkdir -p $(CONTENTS)/MacOS
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

$(ICNS): icon/$(APP).iconset/icon_512x512@2x.png
	@mkdir -p Resources
	iconutil -c icns icon/$(APP).iconset -o $@

icon: $(ICNS)

$(BUNDLE): $(BIN) Info.plist Resources/player.html Resources/ate.png $(ICNS)
	@mkdir -p $(CONTENTS)/Resources
	cp Info.plist $(CONTENTS)/Info.plist
	cp Resources/player.html $(CONTENTS)/Resources/player.html
	cp Resources/*.png $(CONTENTS)/Resources/
	cp $(ICNS) $(CONTENTS)/Resources/$(APP).icns
	printf 'APPL????' > $(CONTENTS)/PkgInfo
	@# ad-hoc signature: no developer account needed
	codesign --force --sign - $(BUNDLE) >/dev/null 2>&1 || true
	@touch $(BUNDLE)
	@echo "built $(BUNDLE)"

run: $(BUNDLE)
	@# `touch .extra-instance` to launch an additional copy (for testing host + guest on one Mac)
	@# the app's stderr (NSLog, AppKit exception reports) lands in build/app.log
	@# `touch .debug` to run with WP_DEBUG tracing straight from the binary instead of `open`
	@if [ -f .extra-instance ]; then rm -f .extra-instance; open -n --stdout $(BUILD)/app2.log --stderr $(BUILD)/app2.log $(BUNDLE); \
	elif [ -f .debug ]; then pkill -x $(APP) 2>/dev/null; sleep 0.5; (WP_DEBUG=1 nohup $(BIN) > $(BUILD)/app.log 2>&1 &) ; \
	else pkill -x $(APP) 2>/dev/null; sleep 0.5; : > $(BUILD)/app.log; open --stdout $(BUILD)/app.log --stderr $(BUILD)/app.log $(BUNDLE); fi

universal:
	$(MAKE) ARCHS="-arch arm64 -arch x86_64" clean-bin all

clean-bin:
	rm -f $(BIN)

test:
	@if [ -f .task ]; then t=$$(cat .task); rm -f .task; echo "== task: $$t"; $(MAKE) $$t; exit $$?; fi
	@mkdir -p $(BUILD)/tests
	$(CC) -std=c99 -Wall -Wextra -O1 -Isrc tests/test_json.c src/wp_json.c -o $(BUILD)/tests/test_json
	$(CC) -std=c99 -Wall -Wextra -O1 -Isrc tests/test_util.c src/wp_util.c -o $(BUILD)/tests/test_util
	$(CC) -std=c99 -Wall -Wextra -O1 -Isrc tests/test_fancy.c src/wp_fancy.c -o $(BUILD)/tests/test_fancy
	$(CC) -std=c99 -Wall -Wextra -O1 -Isrc tests/test_lobby.c src/wp_lobby.c src/wp_util.c -o $(BUILD)/tests/test_lobby
	$(CC) -std=gnu11 -Wall -Wextra -O1 -Isrc tests/test_search.c src/wp_search_parse.c src/wp_json.c src/wp_util.c -o $(BUILD)/tests/test_search
	$(BUILD)/tests/test_json
	$(BUILD)/tests/test_util
	$(BUILD)/tests/test_fancy
	$(BUILD)/tests/test_lobby
	$(BUILD)/tests/test_search
	@if command -v node >/dev/null 2>&1; then node tests/test_player.js; else echo "node not found: skipping player sync test"; fi

# selftest: in-process TLS-PSK check. If .robot-code holds an invite code, it
# instead joins the LIVE room on this Mac as a headless guest ("robot").
selftest: $(BUILD)/robot
	@if [ -f .search-query ]; then $(BUILD)/robot --search "$$(cat .search-query)"; \
	elif [ -f .robot-code ]; then $(BUILD)/robot --join 127.0.0.1:47321 "$$(cat .robot-code)" Robot; \
	else $(BUILD)/robot --selftest && $(BUILD)/robot --selftest-lobby; fi

$(BUILD)/robot: $(SRC) $(HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

# ---- iPhone (ios/: C + UIKit on the same engine) ----
IOS_PROJ := ios/WatchParty.xcodeproj
IOS_DD   := $(BUILD)/ios-dd
IOS_APP  := $(IOS_DD)/Build/Products/Debug-iphonesimulator/WatchParty.app
IOS_ID   := com.fredbackman.watchparty.ios

ios-project:
	python3 ios/make_project.py

ios-build:
	@mkdir -p $(BUILD)
	@xcrun --sdk iphonesimulator --show-sdk-path >/dev/null 2>&1 || { echo "No iOS simulator SDK: install Xcode (and its iOS platform) first."; exit 1; }
	@echo "xcodebuild (simulator, Debug)…"
	@xcodebuild -project $(IOS_PROJ) -scheme WatchParty -sdk iphonesimulator -configuration Debug \
	    -derivedDataPath $(IOS_DD) CODE_SIGNING_ALLOWED=NO build > $(BUILD)/ios-build.log 2>&1 \
	  || { grep -E "error|warning:" $(BUILD)/ios-build.log | sort -u | head -60; tail -25 $(BUILD)/ios-build.log; exit 1; }
	@grep -E "warning:" $(BUILD)/ios-build.log | grep -v "^ld: warning" | sort -u | head -40 || true
	@echo "built $(IOS_APP)"

ios-sim: ios-build
	sh ios/sim.sh $(IOS_APP) $(IOS_ID) $(BUILD)/ios-shot.png

clean:
	rm -rf $(BUILD) .robot-code .search-query .extra-instance .debug .task Makefile.bak
