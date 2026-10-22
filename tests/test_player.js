// Tests the sync controller embedded in Resources/player.html with a fake
// YouTube player and a fake clock. Run: node tests/test_player.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '..', 'Resources', 'player.html'), 'utf8');
const m = html.match(/\/\*SYNC-BEGIN\*\/([\s\S]*?)\/\*SYNC-END\*\//);
if (!m) { console.log('FAIL: SYNC block not found'); process.exit(1); }
const ctx = {};
vm.createContext(ctx);
vm.runInContext(m[1] + '\nthis.createSync = createSync;', ctx);
const createSync = ctx.createSync;

const S = { UNSTARTED: -1, ENDED: 0, PLAYING: 1, PAUSED: 2, BUFFERING: 3, CUED: 5 };
let fails = 0;
function check(cond, msg) { if (!cond) { fails++; console.log('FAIL: ' + msg); } }

function harness() {
  const h = { clock: 1000000, posts: [], calls: [] };
  const player = {
    t: 0, s: S.UNSTARTED, rate: 1,
    getCurrentTime() { return this.t; },
    getPlayerState() { return this.s; },
    getPlaybackRate() { return this.rate; },
    playVideo() { h.calls.push('play'); },
    pauseVideo() { h.calls.push('pause'); },
    seekTo(t) { h.calls.push('seek:' + t); this.t = t; },
    loadVideoById(id, t) { h.calls.push('load:' + id + ':' + (t || 0)); this.t = t || 0; this.s = S.UNSTARTED; },
    cueVideoById(id, t) { h.calls.push('cue:' + id + ':' + (t || 0)); this.t = t || 0; this.s = S.CUED; }
  };
  h.player = player;
  h.sync = createSync({
    post: (o) => h.posts.push(o),
    now: () => h.clock,
    player: player,
    onVideoLoaded: () => h.calls.push('loaded')
  });
  // advance wall clock by ms; the player advances when playing
  h.tick = (ms, polls = true) => {
    const steps = Math.max(1, Math.round(ms / 250));
    for (let i = 0; i < steps; i++) {
      h.clock += ms / steps;
      if (player.s === S.PLAYING) player.t += (ms / steps / 1000) * player.rate;
      if (polls) h.sync.poll();
    }
  };
  h.event = (s) => { player.s = s; h.sync.onStateChange(s); };
  h.postsOf = (ev) => h.posts.filter(p => p.ev === ev);
  h.reset = () => { h.posts.length = 0; h.calls.length = 0; };
  return h;
}

// 1. load before ready is queued and applied on ready
{
  const h = harness();
  h.sync.load('dQw4w9WgXcQ', 0, true);
  check(h.calls.length === 0, 'load before ready should not touch the player');
  h.sync.onReady();
  check(h.postsOf('ready').length === 1, 'ready posted');
  check(h.calls.includes('load:dQw4w9WgXcQ:0'), 'queued load applied after ready');
  // the autoplay transitions should be swallowed
  h.event(S.UNSTARTED); h.event(S.BUFFERING); h.tick(500); h.event(S.PLAYING);
  check(h.postsOf('state').length === 0, 'load autoplay events are not echoed');
  h.tick(3000);
  check(h.postsOf('state').length === 0 && h.postsOf('seek').length === 0, 'steady playback produces no events');
  check(h.postsOf('tick').length >= 2, 'ticks are posted while playing');
}

// 2. user presses play from cued -> state event; then pause -> state event
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, false);
  check(h.calls.includes('cue:abcdefghijk:0'), 'paused load cues the video');
  h.event(S.CUED);
  h.tick(5000);
  h.reset();
  h.event(S.BUFFERING); h.tick(300); h.event(S.PLAYING);
  const st = h.postsOf('state');
  check(st.length === 1 && st[0].s === S.PLAYING, 'user play reported once');
  h.tick(2000);
  h.reset();
  h.event(S.PAUSED);
  const st2 = h.postsOf('state');
  check(st2.length === 1 && st2[0].s === S.PAUSED && Math.abs(st2[0].pos - 2.0) < 0.01, 'user pause reported with position');
}

// 3. user seeks while playing (buffering in between) -> seek event, no state event
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, true);
  h.event(S.PLAYING);
  h.tick(5000);
  h.reset();
  h.event(S.BUFFERING);
  h.player.t = 120;
  h.tick(1500);              // buffering for a while
  h.event(S.PLAYING);
  const sk = h.postsOf('seek');
  check(sk.length === 1 && Math.abs(sk[0].pos - 120) < 0.01 && sk[0].playing === true, 'seek while playing reported');
  check(h.postsOf('state').length === 0, 'no spurious state event on seek');
  h.tick(2000);
  check(h.postsOf('seek').length === 1, 'no repeated seek events');
}

// 4. user seeks while paused -> seek event with playing=false
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 10, false);
  h.event(S.CUED);
  h.tick(5000);
  h.event(S.PLAYING); h.tick(1000); h.event(S.PAUSED);
  h.reset();
  h.player.t = 42;
  h.event(S.BUFFERING); h.tick(250); h.event(S.PAUSED);
  const sk = h.postsOf('seek');
  check(sk.length === 1 && sk[0].playing === false && Math.abs(sk[0].pos - 42) < 0.01, 'seek while paused reported');
}

// 5. remote play / pause / seek are applied but not echoed
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, false);
  h.event(S.CUED);
  h.tick(5000);
  h.reset();
  h.sync.play(30);
  check(h.calls.includes('seek:30') && h.calls.includes('play'), 'remote play seeks and plays');
  h.event(S.BUFFERING); h.tick(400); h.event(S.PLAYING);
  h.tick(1000);
  check(h.postsOf('state').length === 0 && h.postsOf('seek').length === 0, 'remote play not echoed');
  h.reset();
  h.sync.pause(31.5);
  h.event(S.PAUSED);
  h.tick(1000);
  check(h.calls.includes('pause') && !h.calls.some(c => c.startsWith('seek')), 'remote pause near position does not seek');
  check(h.postsOf('state').length === 0, 'remote pause not echoed');
  h.reset();
  h.sync.seek(200, true);
  h.event(S.BUFFERING); h.tick(300); h.event(S.PLAYING);
  h.tick(1000);
  check(h.calls.includes('seek:200') && h.calls.includes('play'), 'remote seek applied');
  check(h.postsOf('seek').length === 0 && h.postsOf('state').length === 0, 'remote seek not echoed');
}

// 6. slow remote apply: PLAYING arrives after the suppress window — still no echo
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, false);
  h.event(S.CUED);
  h.tick(5000);
  h.reset();
  h.sync.play(50);
  h.event(S.BUFFERING);
  h.tick(4000);              // longer than the 1.5 s suppress window
  h.event(S.PLAYING);
  h.tick(1000);
  check(h.postsOf('state').length === 0 && h.postsOf('seek').length === 0, 'late PLAYING after remote play is not echoed');
}

// 7. applyTick corrections
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, true);
  h.event(S.PLAYING);
  h.tick(10000);
  h.reset();
  h.sync.applyTick(10.5, true, false);
  check(!h.calls.some(c => c.startsWith('seek')), 'small drift ignored');
  h.sync.applyTick(20, true, false);
  check(h.calls.includes('seek:20'), 'large drift corrected');
  h.tick(1000);
  check(h.postsOf('seek').length === 0, 'drift correction not echoed');
  h.reset();
  h.sync.applyTick(21, false, false);
  check(h.calls.includes('pause'), 'host paused -> we pause');
  h.event(S.PAUSED);
  h.reset();
  h.sync.applyTick(21, true, false);
  check(h.calls.includes('play'), 'host playing -> we play');
  h.event(S.PLAYING);
  h.tick(1000);
  check(h.postsOf('state').length === 0, 'tick-driven play not echoed');
  h.reset();
  h.sync.applyTick(99, true, true);
  check(h.calls.length === 0, 'ended tick does nothing');
}

// 8. user-initiated toggle goes through the normal event path and IS reported
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, true);
  h.event(S.PLAYING);
  h.tick(5000);
  h.reset();
  h.sync.toggle();
  check(h.calls.includes('pause'), 'toggle pauses when playing');
  h.event(S.PAUSED);
  check(h.postsOf('state').length === 1 && h.postsOf('state')[0].s === S.PAUSED, 'toggle pause reported');
}

// 9. playback rate change does not look like a seek
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, true);
  h.event(S.PLAYING);
  h.tick(3000);
  h.player.rate = 2;
  h.reset();
  h.tick(5000);
  check(h.postsOf('seek').length === 0, '2x playback is not a seek');
}

// 10. video end is reported once as state 0 and then stays quiet
{
  const h = harness();
  h.sync.onReady();
  h.sync.load('abcdefghijk', 0, true);
  h.event(S.PLAYING);
  h.tick(5000);
  h.reset();
  h.event(S.ENDED);
  h.tick(3000);
  check(h.postsOf('state').length === 1 && h.postsOf('state')[0].s === S.ENDED, 'ended reported once');
}

if (fails) { console.log('test_player: ' + fails + ' failure(s)'); process.exit(1); }
console.log('test_player: all passed');
