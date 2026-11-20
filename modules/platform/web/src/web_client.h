#pragma once

#include <string_view>

namespace dream::modules::web {

// The page a browser opens to see and play the game (ADR 0039). It fetches each new frame from
// /frame as raw RGBA pixels and paints them on a canvas, and posts keyboard, mouse, wheel and touch
// input to /input as lines of text:
//
//   k <KeyboardEvent.code> <1 pressed | 0 released>
//   m <x> <y>                      pointer position, in the game's pixels
//   b <0 left | 1 right | 2 middle | 3 touch> <1 | 0>
//   w <x> <y>                      wheel, in lines, positive y up
//   t <text, percent-encoded>      typed text
//
// It uses nothing from the network: the whole page is this string.
inline constexpr std::string_view k_client_page = R"dream_html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DreamEngine</title>
<style>
  :root {
    --bg: #f4f5f7; --panel: #ffffff; --text: #1d2330; --muted: #5b6475; --border: #d8dce3;
    --accent: #3b5bdb; --danger: #c92a2a; --ok: #2b8a3e;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #12151b; --panel: #1b1f27; --text: #e6e9ef; --muted: #9aa3b2; --border: #2c3340;
      --accent: #748ffc; --danger: #ff6b6b; --ok: #69db7c;
    }
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
    font: 14px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, sans-serif; }
  body { display: flex; flex-direction: column; }
  header { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 16px; padding: 8px 16px;
    background: var(--panel); border-bottom: 1px solid var(--border); }
  h1 { font-size: 16px; margin: 0; font-weight: 600; }
  .stat { color: var(--muted); font-variant-numeric: tabular-nums; }
  .spacer { flex: 1; }
  button { font: inherit; padding: 4px 12px; border-radius: 6px; border: 1px solid var(--border);
    background: var(--panel); color: var(--text); cursor: pointer; }
  button:hover { border-color: var(--accent); }
  button.danger { color: var(--danger); }
  main { flex: 1; display: flex; align-items: center; justify-content: center; padding: 16px;
    min-height: 0; }
  canvas { max-width: 100%; max-height: 100%; background: #000; outline: none;
    box-shadow: 0 2px 12px rgba(0,0,0,.25); image-rendering: auto; touch-action: none; }
  canvas:focus { box-shadow: 0 0 0 2px var(--accent), 0 2px 12px rgba(0,0,0,.25); }
  #note { padding: 0 16px 12px; color: var(--muted); text-align: center; }
  #state.ok { color: var(--ok); } #state.bad { color: var(--danger); }
</style>
</head>
<body>
<header>
  <h1 id="title">DreamEngine</h1>
  <span id="state" class="stat">connecting…</span>
  <span id="fps" class="stat"></span>
  <span class="spacer"></span>
  <button id="full" type="button">Full screen</button>
  <button id="quit" type="button" class="danger">Stop game</button>
</header>
<main><canvas id="screen" tabindex="0" width="16" height="9"></canvas></main>
<div id="note">Click the picture to give it the keyboard. Drawn on the CPU by DreamEngine's software renderer.</div>
<script>
"use strict";
const canvas = document.getElementById("screen");
const context = canvas.getContext("2d", { alpha: false });
const stateLabel = document.getElementById("state");
const fpsLabel = document.getElementById("fps");
let frameId = 0, shown = 0, lastFpsTime = performance.now(), running = true;
let image = null;

function setState(text, kind) {
  stateLabel.textContent = text;
  stateLabel.className = "stat " + (kind || "");
}

async function loadStatus() {
  try {
    const status = await (await fetch("/status")).json();
    document.title = status.title;
    document.getElementById("title").textContent = status.title;
  } catch (e) { /* The frames will say whether the game is there. */ }
}

async function frameLoop() {
  while (running) {
    try {
      const response = await fetch("/frame?after=" + frameId, { cache: "no-store" });
      if (response.status === 204) { continue; }
      if (!response.ok) { throw new Error("HTTP " + response.status); }
      const width = parseInt(response.headers.get("X-Width"), 10);
      const height = parseInt(response.headers.get("X-Height"), 10);
      frameId = parseInt(response.headers.get("X-Frame"), 10);
      const pixels = new Uint8ClampedArray(await response.arrayBuffer());
      if (pixels.length !== width * height * 4) { continue; }
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width; canvas.height = height; image = null;
      }
      if (!image) { image = context.createImageData(width, height); }
      image.data.set(pixels);
      context.putImageData(image, 0, 0);
      ++shown;
      setState("running", "ok");
    } catch (e) {
      setState("game not reachable — is it still running?", "bad");
      await new Promise(resolve => setTimeout(resolve, 1000));
    }
  }
}

setInterval(() => {
  const now = performance.now();
  fpsLabel.textContent = (shown * 1000 / (now - lastFpsTime)).toFixed(1) + " frames/s shown";
  shown = 0; lastFpsTime = now;
}, 1000);

// Input, sent in batches so a burst of mouse moves costs one request.
let queue = [], sending = false;
function send(line) { queue.push(line); flush(); }
async function flush() {
  if (sending || queue.length === 0) { return; }
  sending = true;
  const body = queue.join("\n"); queue = [];
  try { await fetch("/input", { method: "POST", body }); } catch (e) { /* dropped */ }
  sending = false;
  if (queue.length) { flush(); }
}

function toGame(event) {
  const box = canvas.getBoundingClientRect();
  const x = (event.clientX - box.left) * canvas.width / box.width;
  const y = (event.clientY - box.top) * canvas.height / box.height;
  return x.toFixed(1) + " " + y.toFixed(1);
}

const held = new Set();
function keyChange(event, pressed) {
  if (document.activeElement !== canvas) { return; }
  if (event.code && !(pressed && event.repeat)) {
    if (pressed) { held.add(event.code); } else { held.delete(event.code); }
    send("k " + event.code + " " + (pressed ? 1 : 0));
  }
  if (pressed && event.key && event.key.length === 1 && !event.ctrlKey && !event.metaKey) {
    send("t " + encodeURIComponent(event.key));
  }
  // Keeps the page from scrolling or the browser from leaving it.
  if (!event.ctrlKey && !event.metaKey && event.code !== "F5" && event.code !== "F11" &&
      event.code !== "F12") {
    event.preventDefault();
  }
}
window.addEventListener("keydown", event => keyChange(event, true));
window.addEventListener("keyup", event => keyChange(event, false));
canvas.addEventListener("blur", () => {
  for (const code of held) { send("k " + code + " 0"); }
  held.clear();
});

function buttonOf(event) {
  if (event.pointerType === "touch") { return 3; }
  return event.button === 2 ? 1 : event.button === 1 ? 2 : 0;
}
canvas.addEventListener("pointermove", event => send("m " + toGame(event)));
canvas.addEventListener("pointerdown", event => {
  canvas.focus();
  canvas.setPointerCapture(event.pointerId);
  send("m " + toGame(event));
  send("b " + buttonOf(event) + " 1");
  event.preventDefault();
});
canvas.addEventListener("pointerup", event => {
  send("m " + toGame(event));
  send("b " + buttonOf(event) + " 0");
});
canvas.addEventListener("pointercancel", event => send("b " + buttonOf(event) + " 0"));
canvas.addEventListener("wheel", event => {
  const lines = event.deltaMode === 0 ? 1 / 40 : 1;
  send("w " + (event.deltaX * lines).toFixed(2) + " " + (-event.deltaY * lines).toFixed(2));
  event.preventDefault();
}, { passive: false });
canvas.addEventListener("contextmenu", event => event.preventDefault());

document.getElementById("full").addEventListener("click", () => {
  if (canvas.requestFullscreen) { canvas.requestFullscreen(); }
  canvas.focus();
});
document.getElementById("quit").addEventListener("click", async () => {
  if (!confirm("Stop the game? It ends, and this page stops updating.")) { return; }
  try { await fetch("/quit", { method: "POST" }); } catch (e) { /* already gone */ }
  running = false;
  setState("stopped", "bad");
});

loadStatus();
frameLoop();
canvas.focus();
</script>
</body>
</html>
)dream_html";

} // namespace dream::modules::web
