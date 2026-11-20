#pragma once

#include <string_view>

namespace hub {

// The hub's page: games, tests, screenshots and the output of builds and test runs. It asks the
// hub's /api for everything, and uses nothing from the network.
inline constexpr std::string_view k_page = R"hub_html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DreamEngine Hub</title>
<style>
  :root {
    --bg: #f4f5f7; --panel: #ffffff; --text: #1d2330; --muted: #5b6475; --border: #d8dce3;
    --accent: #3b5bdb; --accent-text: #ffffff; --ok: #2b8a3e; --bad: #c92a2a; --warn: #a15c00;
    --code: #eef0f4;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #12151b; --panel: #1b1f27; --text: #e6e9ef; --muted: #9aa3b2; --border: #2c3340;
      --accent: #748ffc; --accent-text: #10131a; --ok: #69db7c; --bad: #ff6b6b; --warn: #ffc078;
      --code: #232833;
    }
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text);
    font: 14px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, sans-serif; }
  header { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 16px; padding: 12px 16px;
    background: var(--panel); border-bottom: 1px solid var(--border); }
  h1 { margin: 0; font-size: 18px; }
  h2 { font-size: 15px; margin: 0 0 8px; }
  .muted { color: var(--muted); }
  nav { display: flex; gap: 4px; padding: 8px 16px 0; overflow-x: auto; }
  nav button { border: 1px solid var(--border); border-bottom: none; border-radius: 8px 8px 0 0;
    background: transparent; color: var(--muted); padding: 8px 14px; font: inherit; cursor: pointer; }
  nav button.active { background: var(--panel); color: var(--text); font-weight: 600; }
  section { display: none; margin: 0 16px 16px; padding: 16px; background: var(--panel);
    border: 1px solid var(--border); border-radius: 0 8px 8px 8px; }
  section.active { display: block; }
  button.action { font: inherit; padding: 5px 12px; border-radius: 6px; cursor: pointer;
    border: 1px solid var(--accent); background: var(--accent); color: var(--accent-text); }
  button.plain { font: inherit; padding: 5px 12px; border-radius: 6px; cursor: pointer;
    border: 1px solid var(--border); background: var(--panel); color: var(--text); }
  button:disabled { opacity: .5; cursor: default; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }
  .card { border: 1px solid var(--border); border-radius: 8px; padding: 12px; display: flex;
    flex-direction: column; gap: 8px; }
  .card .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .ok { color: var(--ok); } .bad { color: var(--bad); } .warn { color: var(--warn); }
  pre { background: var(--code); border-radius: 6px; padding: 10px; overflow: auto; max-height: 420px;
    font: 12px/1.4 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; white-space: pre-wrap;
    word-break: break-word; margin: 8px 0 0; }
  table { border-collapse: collapse; width: 100%; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--border);
    vertical-align: top; }
  td.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
  tr.clickable { cursor: pointer; }
  .toolbar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 12px; }
  input[type=text] { font: inherit; padding: 5px 8px; border-radius: 6px; border: 1px solid var(--border);
    background: var(--bg); color: var(--text); min-width: 0; flex: 1 1 200px; max-width: 360px; }
  .shots { display: grid; gap: 16px; }
  .pair { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 8px; }
  .pair img { width: 100%; border: 1px solid var(--border); border-radius: 6px; background: #000; }
  .summary { display: flex; gap: 16px; flex-wrap: wrap; margin-bottom: 8px; font-weight: 600; }
  a { color: var(--accent); }
</style>
</head>
<body>
<header>
  <h1>DreamEngine Hub</h1>
  <span class="muted">Build, play and test in the browser — no GPU, display or sound needed.</span>
</header>
<nav>
  <button data-tab="games" class="active">Games</button>
  <button data-tab="tests">Tests</button>
  <button data-tab="shots">Screenshots</button>
  <button data-tab="build">Build</button>
</nav>

<section id="games" class="active">
  <p class="muted">Each game runs as its own program and serves its own page. Start one, then open it.</p>
  <div id="game-list" class="grid"></div>
  <pre id="game-log" hidden></pre>
</section>

<section id="tests">
  <div class="toolbar">
    <button class="action" id="run-tests">Run tests</button>
    <input type="text" id="filter" placeholder="Only tests matching (regular expression)">
    <span id="test-state" class="muted"></span>
  </div>
  <div id="test-summary" class="summary"></div>
  <table>
    <thead><tr><th>Test</th><th>Result</th><th class="num">Seconds</th></tr></thead>
    <tbody id="test-rows"><tr><td colspan="3" class="muted">No results yet. Run the tests.</td></tr></tbody>
  </table>
  <h2 style="margin-top:16px">Output</h2>
  <pre id="test-log" class="muted">Click a test to see its output, or run the tests to follow them here.</pre>
</section>

<section id="shots">
  <p class="muted">Pictures the screenshot tests captured, beside the references they are compared with.</p>
  <div class="toolbar"><button class="plain" id="refresh-shots">Refresh</button></div>
  <div id="shot-list" class="shots"></div>
</section>

<section id="build">
  <div class="toolbar">
    <button class="action" id="run-build">Build everything</button>
    <span id="build-state" class="muted"></span>
  </div>
  <p class="muted">Folder: <code id="build-dir"></code></p>
  <pre id="build-log">Nothing built from here yet.</pre>
</section>

<script>
"use strict";
const $ = id => document.getElementById(id);
let state = null, logOf = null, testResults = [];

for (const button of document.querySelectorAll("nav button")) {
  button.addEventListener("click", () => {
    for (const other of document.querySelectorAll("nav button")) { other.classList.remove("active"); }
    for (const section of document.querySelectorAll("section")) { section.classList.remove("active"); }
    button.classList.add("active");
    $(button.dataset.tab).classList.add("active");
    if (button.dataset.tab === "shots") { loadShots(); }
    if (button.dataset.tab === "tests") { loadResults(); }
  });
}

async function post(url) {
  const response = await fetch(url, { method: "POST" });
  if (!response.ok) { alert(await response.text()); }
  await refresh();
}

function el(tag, attributes, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(attributes || {})) {
    if (key === "class") { node.className = value; }
    else if (key.startsWith("on")) { node.addEventListener(key.slice(2), value); }
    else { node.setAttribute(key, value); }
  }
  for (const child of children) { node.append(child); }
  return node;
}

function renderGames() {
  const list = $("game-list");
  list.replaceChildren();
  for (const game of state.games) {
    const address = location.protocol + "//" + location.hostname + ":" + game.port + "/";
    const actions = el("div", { class: "row" });
    if (game.running) {
      actions.append(el("a", { href: address, target: "_blank", rel: "noopener" }, "Open game ↗"));
      actions.append(el("button", { class: "plain", onclick: () => post("/api/game/stop?name=" + encodeURIComponent(game.name)) }, "Stop"));
    } else {
      const start = el("button", { class: "action", onclick: () => post("/api/game/start?name=" + encodeURIComponent(game.name)) }, "Start");
      if (!game.built) { start.disabled = true; start.title = "Build it first"; }
      actions.append(start);
    }
    actions.append(el("button", { class: "plain", onclick: () => { logOf = game.name; showLog(); } }, "Log"));
    const status = game.built ? game.state : "not built";
    list.append(el("div", { class: "card" },
      el("h2", {}, game.title),
      el("div", { class: "muted" }, game.name),
      el("div", { class: game.running ? "ok" : (status.startsWith("exited with code 0") || status === "not started" ? "muted" : "warn") }, status),
      actions));
  }
  if (!state.games.length) {
    list.append(el("p", { class: "muted" }, "No games found. Build the project first, then reload this page."));
  }
}

async function showLog() {
  if (!logOf) { return; }
  const text = await (await fetch("/api/log?of=" + encodeURIComponent(logOf))).text();
  const pre = $("game-log");
  pre.hidden = false;
  pre.textContent = "— " + logOf + " —\n" + (text || "(no output yet)");
  pre.scrollTop = pre.scrollHeight;
}

function renderTask() {
  const task = state.task;
  const running = task && task.state === "running";
  $("run-tests").disabled = running;
  $("run-build").disabled = running;
  $("build-dir").textContent = state.build;
  const describe = kind => !task || task.kind !== kind ? "" :
    (running ? "running for " + task.seconds + " s…" : task.state + " after " + task.seconds + " s");
  $("test-state").textContent = describe("tests");
  $("build-state").textContent = describe("build");
}

let lastTaskState = "";
async function followTask() {
  const task = state.task;
  if (!task) { return; }
  const key = task.kind + task.state;
  if (task.state === "running" || key !== lastTaskState) {
    const text = await (await fetch("/api/log?of=task")).text();
    const target = task.kind === "build" ? $("build-log") : $("test-log");
    target.classList.remove("muted");
    target.textContent = text || "(no output yet)";
    target.scrollTop = target.scrollHeight;
    if (task.state !== "running" && task.kind === "tests") { loadResults(); }
  }
  lastTaskState = key;
}

async function loadResults() {
  const results = await (await fetch("/api/tests/results")).json();
  testResults = results.tests;
  const rows = $("test-rows");
  rows.replaceChildren();
  if (!results.available) {
    rows.append(el("tr", {}, el("td", { colspan: 3, class: "muted" }, "No results yet. Run the tests.")));
    $("test-summary").replaceChildren();
    return;
  }
  const order = { failed: 0, skipped: 1, passed: 2 };
  testResults.sort((a, b) => order[a.status] - order[b.status] || a.name.localeCompare(b.name));
  const count = status => testResults.filter(test => test.status === status).length;
  $("test-summary").replaceChildren(
    el("span", { class: "ok" }, count("passed") + " passed"),
    el("span", { class: count("failed") ? "bad" : "muted" }, count("failed") + " failed"),
    el("span", { class: "muted" }, count("skipped") + " skipped"));
  for (const test of testResults) {
    const kind = test.status === "passed" ? "ok" : test.status === "failed" ? "bad" : "warn";
    rows.append(el("tr", { class: "clickable", onclick: () => {
        $("test-log").classList.remove("muted");
        $("test-log").textContent = "— " + test.name + " —\n" + (test.output || "(no output)");
      } },
      el("td", {}, test.name), el("td", { class: kind }, test.status),
      el("td", { class: "num" }, test.seconds.toFixed(2))));
  }
}

async function loadShots() {
  const shots = await (await fetch("/api/screenshots")).json();
  const list = $("shot-list");
  list.replaceChildren();
  if (!shots.length) {
    list.append(el("p", { class: "muted" }, "No screenshots yet. Run the tests: the screenshot checks save their captures in the build folder."));
  }
  for (const shot of shots) {
    const pair = el("div", { class: "pair" });
    pair.append(el("figure", { style: "margin:0" }, el("img", { src: "/file?path=" + encodeURIComponent(shot.capture), alt: "Capture" }),
      el("figcaption", { class: "muted" }, "Captured: " + shot.capture)));
    if (shot.reference) {
      pair.append(el("figure", { style: "margin:0" }, el("img", { src: "/file?path=" + encodeURIComponent(shot.reference), alt: "Reference" }),
        el("figcaption", { class: "muted" }, "Reference: " + shot.reference)));
    }
    list.append(el("div", {}, el("h2", {}, shot.sample || "screenshot"), pair));
  }
}

$("run-tests").addEventListener("click", () => {
  const filter = $("filter").value.trim();
  post("/api/tests/run" + (filter ? "?filter=" + encodeURIComponent(filter) : ""));
});
$("run-build").addEventListener("click", () => post("/api/build"));
$("refresh-shots").addEventListener("click", loadShots);

async function refresh() {
  try {
    state = await (await fetch("/api/state")).json();
    renderGames();
    renderTask();
    await followTask();
    if (logOf) { await showLog(); }
  } catch (e) { /* The hub stopped; keep trying. */ }
}
refresh();
setInterval(refresh, 1500);
</script>
</body>
</html>
)hub_html";

} // namespace hub
