# WatchParty

A Mac and iPhone app for watching YouTube **together**: one person hosts a
room, friends on the same Wi‑Fi see it and **ask to join** (or use an invite
code from anywhere), and play / pause / seek stay in sync for everyone. The window is nothing but the video — borderless, floating, 16:9 —
with translucent controls that only appear when you reach for them, a chat
column, subtitle‑style messages, and animated reaction stickers.

Written in plain **C** — the Cocoa (Mac) and UIKit (iPhone) UIs are driven
through the Objective‑C runtime (`objc_msgSend`), the player is a `WKWebView`
running YouTube's IFrame player, and the room network is Apple's
Network.framework with TLS. Both apps share one room engine (`src/wp_room.c`),
so a Mac and an iPhone can be in the same room.

```
  ╭──────────────────────────────────────────────────────────────────╮
  │        ╭ ⊗ [ YouTube link ] [Load] [Host…][Join…][Invite][Leave] [Chat: on] ╮   │
  │        ╰ ● Hosting “Movie night” · 3 watching · code …  ╯   (pointer here)   │
  │                                                      ╭──────────────╮ │
  │                                                      │ Fred, Bob    │ │
  │                 YouTube player                       │ chat, live   │ │
  │                                                      │ 🗿 sigma 🔥 W │ │
  │              🔥 W  (reaction floats up)              │ 😂 ❤️ 🔥 👏  │ │
  │                                                      │ [ message  ] │ │
  ╰──────────────────────────────────────────────────────┴──────────────┴─╯
```

## Build & run

Requirements: macOS 11 or newer, Xcode Command Line Tools (`xcode-select --install`).

```sh
make            # builds build/WatchParty.app
make run        # builds and launches it
make test       # unit tests (JSON, utilities, player sync logic — needs node for the last one)
make selftest   # end-to-end check of the encrypted transport, no UI
make universal  # arm64 + Intel binary
```

Drag `build/WatchParty.app` to `/Applications` if you like. The app is
ad‑hoc signed, which is fine on the Mac that built it; to hand it to another
Mac, right‑click ▸ Open the first time (or sign it with a Developer ID).

## Using it

**The window** is just the video. Move the pointer to the **top centre** and
the control bar fades in: link field, Load, Host / Join / Invite / Leave,
the chat mode button, and a status line with the room's code and address.
Move away and it fades out; Esc hides it at once. Drag the bar to move the
window; resize from the window's edges (it keeps 16:9); ⌘‑Ctrl‑F for full
screen; ⇧⌘T toggles "float on top". The ⊗ (or ⌘W) closes the window — the
app stays in the menu bar; click its icon there or in the Dock to get it back.

**Host:** Room ▸ *Host a Room…* (⌘N). You get a 24‑character invite code and
your address. Room ▸ *Copy Invite* (⌘I) puts both on the clipboard.

**Rooms on your Wi‑Fi:** WatchParty keeps an eye out for rooms on the same
network. When one appears you get a card on the left — *“Movie night” is on
your Wi‑Fi* — with **Ask to join**. Rooms (the bar button, or ⌘J) lists them
all any time; Room ▸ *Ask to Join* ▸ has them as a menu too.

**Ask to join:** no code needed. The host gets a card — *Bob wants to join ·
check code 4821* — with **Let in** / **Not now**, plus a Dock badge and a
bounce. Both screens show the same 4‑digit check code: if Bob's screen says
4821 too, it's really Bob (and nobody is sitting in the middle). Let in, and
Bob's app joins the room by itself. Requests that aren't answered expire after
90 seconds; the host sees at most three at a time.

**Join with a code:** Room ▸ *Join with Invite Code…* (⇧⌘J), or *Code…* next
to a room. From anywhere else, type the host's address (`host` or
`host:port`) and the code.

**Watch:** type what you're looking for in the bar and press Return — YouTube
results appear under it; click one (Return again plays the top hit). Or paste
a link (⌘L jumps to the field). Everyone loads it. Anyone can play, pause or scrub with the player's
own controls; the others follow. Video ▸ *Sync Now* (⌘R) snaps to the host's
position (the host also sends its position every 4 s).

**Chat** has three modes — the bar's *Chat* button or ⇧⌘C cycles them, the
Chat menu sets them directly:

| Mode | What you see |
|---|---|
| **Chat column** (default) | A translucent column on the right that stays on. Messages scroll in live; type at the bottom (Return anywhere focuses it); a row of favourite emoji and six quick reactions sit above the box. *Hide* in its header switches to… |
| **Chat as subtitles** | No column. Each message appears once, centred across the **top** of the picture, one at a time, then fades. Reactions still pop up. |
| **Hide chat** | Nothing is shown (messages are still kept; switch back any time). |

**Reactions** are animated stickers in the lingo of the moment — 🗿 sigma,
✨ +1000 aura, 📉 −aura, 🔥 W, 💀 L, 🧢 cap, 🤷 6‑7, 🎯 locked in, 🐐 GOAT,
🧠 brainrot, 😮‍💨 rizz, 🫡 say less, 🍳 cooked, 🚽 skibidi, 🍝 bussin,
💅 it's giving, 🌈 delulu, 🤢 the ick, 🏆 ate, 🧍 NPC, 🫨 crash out,
🌾 touch grass, 👀 chat, is this real, 🥶 no cap. They pop up over the video
for everyone in the room, bounce, and float away. The first six are buttons
in the chat column; all of them are under Chat ▸ *Send Reaction*, the first
nine on ⌥⌘1…9. The list lives in `REACTIONS[]` in `src/wp_app.c` — edit
freely.

**Favourite emoji** (the row above the message box) are 😂 ❤️ 🔥 👏 😮 😢 👍 🎉
by default; set your own with

```sh
defaults write com.fredbackman.watchparty favoriteEmoji "🍿 🤣 🫶 🙌"
```

(Ctrl‑⌘‑Space opens the full emoji picker inside any text field.)

**Font and colour** for chat and subtitles: Chat ▸ *Font* (System, System
Rounded, System Serif, Monospaced, Avenir Next, Futura, Georgia, Chalkboard,
Marker Felt, American Typewriter) and Chat ▸ *Colour* (White, Subtitle
Yellow, Cyan, Lime, Pink, Orange, Lavender, Mint). Both are remembered.

### Joining over the internet

The host's Mac must be reachable on TCP port **47321** (forward that port on
the router to the host's Mac, or use a VPN / Tailscale). Guests then enter
the host's public address and the invite code. If 47321 is busy the app
picks another port and shows it in the status line.

## iPhone & iPad

`ios/` is the iPhone app: the same engine, a UIKit face in C (`ios/wp_ios.c`).
Portrait shows the video with the chat underneath (cards for requests, a strip
of emoji and reactions, a message box); turn the phone sideways for full‑screen
video with the chat as subtitles. 👥 opens the rooms on your Wi‑Fi; ⋯ has Host,
Join with Code, Share Invite, Sync, Fancy Words and your name.

To put it on your phone you need a Mac with **Xcode** (free, from the App Store):

1. Open `ios/WatchParty.xcodeproj`.
2. Xcode ▸ Settings ▸ Accounts: add your Apple ID (a free one works).
3. Click the **WatchParty** target ▸ *Signing & Capabilities* ▸ *Team*: pick
   your name. If Xcode says the bundle ID is taken, change
   `com.fredbackman.watchparty.ios` to something of your own.
4. Plug in the iPhone (unlock it, tap *Trust*), choose it at the top of the
   Xcode window, press ▶.
5. First time only, on the phone: Settings ▸ General ▸ *VPN & Device
   Management* ▸ your Apple ID ▸ *Trust*. On iOS 16+ also turn on Settings ▸
   Privacy & Security ▸ *Developer Mode* (the phone restarts).
6. Open WatchParty and allow *Local Network* when asked — that's how it finds
   rooms.

With a free Apple ID the app runs for 7 days, then press ▶ in Xcode again. A
paid Apple Developer account ($99/yr) gives a year and lets you hand it to
friends through TestFlight. `make ios-sim` builds it for the iOS Simulator.

Notes: iOS pauses apps in the background, so keep WatchParty open while you
host from a phone (it stops the screen from locking while a video plays).

## What "secure" means here

* Every room connection is **TLS 1.2 with a pre‑shared key** derived from
  the invite code (`TLS_PSK_WITH_AES_256_GCM_SHA384`). The code is 120 bits
  of randomness, generated fresh every time a room is hosted. Without it a
  client cannot complete the handshake, so it cannot read, inject or replay
  anything. `make selftest` demonstrates this: a client with the right code
  gets in, one with a wrong code is rejected.
* **Ask to join** never sends the room key in the clear. The guest and the
  host do a fresh P‑256 Diffie–Hellman exchange through a separate "door"
  (`_wplobby._tcp`); the room key travels sealed with the resulting key
  (encrypted and authenticated), only after the host taps *Let in*. The
  4‑digit check code both screens show is derived from that exchange, so if
  someone tried to sit in the middle the codes would not match.
* The host relays every message and stamps it with the sender's registered
  name, so nobody can impersonate someone else in chat.
* No accounts, no server in the middle, no telemetry. Bonjour only announces
  the room's name on the local network.

What it does **not** hide: each viewer streams the video from YouTube
directly, so YouTube sees each of you as usual; guests learn the host's IP
address (they connect to it); the invite code *is* the key, so share it only
with the people you want in the room, and re‑host for a fresh one.

## How it works

| File | Role |
|---|---|
| `src/wp_room.c` | The room engine both apps share: hosting, joining, ask‑to‑join, the sync protocol and clock, chat, reactions |
| `src/wp_app.c` | Mac app: borderless window, overlays (bar, chat column, subtitles, stickers, cards, rooms list, search results), menus, dialogs, menu bar item |
| `ios/wp_ios.c` | iPhone/iPad app: UIKit from C, portrait chat layout, landscape full screen, rooms sheet |
| `src/wp_net.c` | Network.framework transport: TLS‑PSK room listener/connections, the plain "door" for knocking, length‑prefixed JSON frames, Bonjour advertise/browse |
| `src/wp_lobby.c`, `src/wp_ec.c` | Ask‑to‑join crypto: P‑256 ECDH (Security.framework), key derivation, check codes, sealing the room key |
| `src/wp_search.c`, `src/wp_search_parse.c` | YouTube search without an API key (reads the public results page) |
| `src/wp_fancy.c` | Fancy Words: swaps cuss words for the grandest word with the same first letter |
| `src/wp_json.c` | Tiny JSON reader/writer for the flat messages (bounds‑checked, UTF‑8 validated) |
| `src/wp_util.c` | SHA‑256/HMAC, invite codes (base32), YouTube URL parsing, local IP, clock |
| `src/objc_glue.h` | The few macros that make `objc_msgSend` pleasant from C |
| `src/wp_selftest.c` | `--selftest` (in‑process host + clients), `--selftest-lobby` (two processes: ask, get refused, ask again, get let in, compare check codes) and `--join` (headless "robot" guest) |
| `Resources/player.html` | The player page: YouTube IFrame API plus the sync controller (echo suppression, drift correction) |
| `tests/` | C unit tests and a node test that drives the sync controller with a fake player |

Messages are small JSON objects: `hello`/`welcome`, `load`, `play`, `pause`,
`seek`, `chat` (with `react` for stickers), `peers`, `tick` (host position +
host clock), `ping`/`pong` (clock offset). A command you issue is applied
locally and sent; a command you receive is applied with a short "suppress"
window so the resulting player events are not echoed back.

## Troubleshooting

* **"This video is unavailable" / player error 101 or 150** — the video's
  owner disabled embedding. Pick another video.
* **No rooms on your Wi‑Fi** — macOS and iOS ask once for *Local Network*
  permission (Mac: System Settings ▸ Privacy & Security ▸ Local Network;
  iPhone: Settings ▸ Privacy & Security ▸ Local Network). Some guest / office
  Wi‑Fi blocks devices from seeing each other — then use the invite code and
  the host's address.
* **"…is running an older WatchParty"** — that host can't take requests;
  use the invite code, or update their app.
* **"Could not join: secure handshake failed"** — wrong invite code (codes
  change every time a room is hosted).
* **"could not reach the host"** — wrong address, firewall, or the port is
  not forwarded.
* **Play doesn't start on a guest's Mac** — click the video once; a tick from
  the host will keep it aligned afterwards.
* **Where did the window go?** — it is probably behind something or closed;
  click the menu bar icon ▸ *Show WatchParty*.

## Developer helpers

* `make selftest` with an invite code in `.robot-code` joins the live room on
  this Mac as a headless guest and exercises chat / load / pause / sync.
* `touch .extra-instance; make run` opens a second copy for host + guest on
  one Mac; `touch .debug; make run` runs the binary directly with `WP_DEBUG`
  tracing to `build/app.log`.
* `dev-loop.sh` runs fixed `make` targets when a `.request` file appears —
  handy when the Mac is driven remotely through a shared folder.
* Web Inspector is enabled on the player view (right‑click ▸ Inspect Element).
