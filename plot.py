#!/usr/bin/env python3
"""Generate interactive benchmark charts from benchmarks.csv.

Writes into --outdir:
 - interactive.html: uPlot gallery (thumbnail per (color_space, scale_ratio)).
 - chart.html: single-chart detail view with zoom/pan, hover tooltips, and
   a toggle legend. Auto-fits the y-axis to visible data.
 - data.js: the backing dataset.

X axis = commits in chronological order, evenly spaced. Y axis = time_ms,
log scale. One line per backend.

Usage:
    python3 sweep/plot.py [benchmarks.csv] [-o charts]
"""
import argparse
import csv
import json
import os
import re
import subprocess
from collections import defaultdict
from datetime import datetime
from pathlib import Path


_SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def parse_rows(csv_path):
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                yield (
                    datetime.fromisoformat(row["date"]),
                    row["git_revision"],
                    row["color_space"],
                    row["backend"],
                    float(row["scale_ratio"]),
                    float(row["time_ms"]),
                )
            except (ValueError, KeyError, TypeError):
                # Skip partial rows (CSV may be appended to while we read).
                continue


def fetch_commit_meta(shas, git_dir):
    """Return {sha: (author, subject)} in one git call. Missing shas omitted."""
    if not shas:
        return {}
    sep = "\x1f"
    fmt = f"%H{sep}%an{sep}%s"
    cmd = ["git", "-C", str(git_dir), "log", "--no-walk",
           f"--format={fmt}"] + list(shas)
    try:
        out = subprocess.check_output(cmd, text=True, errors="replace")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return {}
    meta = {}
    for line in out.splitlines():
        if not line:
            continue
        parts = line.split(sep, 2)
        if len(parts) == 3:
            sha, author, subject = parts
            meta[sha] = (author, subject)
    return meta


def parse_errors(path):
    """Return list of (date, sha, reason, stderr_text).

    sweep.sh redirects run.sh stderr to errors.log, then appends a one-line
    summary "<sha> <iso_date> <reason>". So the stderr lines preceding a
    summary line belong to that summary's commit.
    """
    if not path.exists():
        return []
    out = []
    pending = []
    for line in path.read_text().splitlines():
        parts = line.strip().split(None, 2)
        if (len(parts) == 3
                and _SHA_RE.match(parts[0])
                and _looks_like_date(parts[1])):
            sha, date_s, reason = parts
            try:
                date = datetime.fromisoformat(date_s)
            except ValueError:
                pending.append(line)
                continue
            stderr = "\n".join(pending).strip()
            out.append((date, sha, reason, stderr))
            pending = []
        else:
            pending.append(line)
    return out


def _looks_like_date(s):
    try:
        datetime.fromisoformat(s)
        return True
    except ValueError:
        return False


def group(rows, errors):
    # (cs, ratio) -> backend -> list[(date, sha, time_ms)]
    grouped = defaultdict(lambda: defaultdict(list))
    succeeded = set()
    for date, sha, cs, backend, ratio, t in rows:
        grouped[(cs, ratio)][backend].append((date, sha, t))
        succeeded.add((date, sha))
    failures = {}  # (date, sha) -> {"reason": str, "stderr": str}
    all_commits = set(succeeded)
    for date, sha, reason, stderr in errors:
        key = (date, sha)
        if key in succeeded:
            continue
        all_commits.add(key)
        failures[key] = {"reason": reason, "stderr": stderr}
    for backends in grouped.values():
        for pts in backends.values():
            pts.sort(key=lambda p: p[0])
    return grouped, sorted(all_commits), failures


UPLOT_COMMON_JS = r"""
const BACKEND_COLORS = {
  scalar: "#1f77b4",
  sse2:   "#ff7f0e",
  avx2:   "#2ca02c",
  neon:   "#d62728",
};
const BACKEND_ORDER = ["scalar", "sse2", "avx2", "neon"];
const DATA = window.BENCH_DATA;

function seriesFor(cs, ratio) {
  const key = cs + "|" + ratio;
  const s = DATA.series[key];
  if (!s) return null;
  const backends = BACKEND_ORDER.filter(b => b in s);
  const xs = DATA.commits.map((_, i) => i);
  const chartData = [xs, ...backends.map(b => s[b])];
  return { backends, data: chartData };
}

function escHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;",
    '"': "&quot;", "'": "&#39;",
  }[c]));
}

// For each series, draw a dashed line that bridges gaps where failed
// commits break the solid line. Painted in the `draw` hook (after all
// series) because at drawAxes time the canvas still gets overwritten
// by the series pass. Bridges only span index-gaps, so they never lie
// on top of solid segments.
function drawBridges(u) {
  const ctx = u.ctx;
  ctx.save();
  ctx.lineWidth = 1;
  ctx.setLineDash([4, 3]);
  ctx.globalAlpha = 0.55;
  for (let si = 1; si < u.series.length; si++) {
    const serie = u.series[si];
    if (!serie.show) continue;
    const ys = u.data[si];
    ctx.strokeStyle = serie.stroke || "#888";
    ctx.beginPath();
    let prev = null;
    for (let i = 0; i < ys.length; i++) {
      if (ys[i] == null) continue;
      if (prev !== null && i > prev.i + 1) {
        ctx.moveTo(u.valToPos(prev.i, "x", true),
                   u.valToPos(prev.v, serie.scale || "y", true));
        ctx.lineTo(u.valToPos(i, "x", true),
                   u.valToPos(ys[i], serie.scale || "y", true));
      }
      prev = { i, v: ys[i] };
    }
    ctx.stroke();
  }
  ctx.restore();
}

// Draw a thin red tick at the bottom of the plot for every failed/skipped
// commit that falls within the current x-scale window.
function drawFailedMarkers(u) {
  const ctx = u.ctx;
  const xMin = u.scales.x.min, xMax = u.scales.x.max;
  const yTop = u.bbox.top + u.bbox.height - 6;
  const yBot = u.bbox.top + u.bbox.height;
  ctx.save();
  ctx.strokeStyle = "rgba(200,0,0,0.55)";
  ctx.lineWidth = 1;
  for (let i = 0; i < DATA.commits.length; i++) {
    if (!DATA.commits[i].reason) continue;
    if (i < xMin || i > xMax) continue;
    const x = u.valToPos(i, "x", true);
    ctx.beginPath();
    ctx.moveTo(x, yTop);
    ctx.lineTo(x, yBot);
    ctx.stroke();
  }
  ctx.restore();
}
"""


INTERACTIVE_HTML = r"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>liboil benchmark charts (interactive)</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/uplot@1.6.27/dist/uPlot.min.css">
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, sans-serif;
         margin: 1.5rem; background: #fafafa; color: #222; }
  h1 { margin: 0 0 0.25rem 0; font-size: 1.3rem; }
  .sub { color: #666; margin: 0 0 1.25rem 0; font-size: 0.9rem; }
  table { border-collapse: collapse; }
  th, td { padding: 4px; vertical-align: middle; }
  th { font-weight: 600; text-align: center; color: #444; }
  th.cs { text-align: right; padding-right: 10px; }

  a.thumb { display: block; position: relative;
            border: 1px solid #ddd; background: #fff;
            width: 320px; height: 180px; text-decoration: none;
            transition: box-shadow 0.08s ease; }
  a.thumb:hover { box-shadow: 0 2px 12px rgba(0,0,0,0.15); }
  a.thumb .uplot { pointer-events: none; }
</style>
</head>
<body>
<h1>liboil benchmark charts (interactive)</h1>
<p class="sub">
  Click a chart for the interactive version with zoom, pan, and
  per-commit hover tooltips.
</p>
<div id="root"></div>

<script src="data.js"></script>
<script src="https://cdn.jsdelivr.net/npm/uplot@1.6.27/dist/uPlot.iife.min.js"></script>
<script>
__COMMON_JS__

function buildGrid() {
  const root = document.getElementById("root");
  const table = document.createElement("table");

  const thead = document.createElement("thead");
  const htr = document.createElement("tr");
  htr.appendChild(document.createElement("th"));
  for (const r of DATA.ratios) {
    const th = document.createElement("th");
    th.textContent = "scale " + r;
    htr.appendChild(th);
  }
  thead.appendChild(htr);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const cs of DATA.color_spaces) {
    const tr = document.createElement("tr");
    const csTh = document.createElement("th");
    csTh.className = "cs";
    csTh.textContent = cs;
    tr.appendChild(csTh);
    for (const r of DATA.ratios) {
      const td = document.createElement("td");
      const link = document.createElement("a");
      link.className = "thumb";
      link.href = "chart.html?cs=" + encodeURIComponent(cs)
                + "&r=" + encodeURIComponent(r);
      td.appendChild(link);
      tr.appendChild(td);
      makeThumb(link, cs, r);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  root.appendChild(table);
}

function makeThumb(container, cs, ratio) {
  const g = seriesFor(cs, ratio);
  if (!g) return;
  const { backends, data } = g;
  const opts = {
    width: 320, height: 180,
    cursor: { show: false },
    legend: { show: false },
    scales: { y: { distr: 3 } },
    axes: [
      { show: false, size: 0 },
      { show: false, size: 0 },
    ],
    padding: [6, 6, 4, 4],
    series: [
      {},
      ...backends.map(b => ({
        label: b,
        stroke: BACKEND_COLORS[b] || "#888",
        width: 1.2,
        points: { show: false },
      })),
    ],
    hooks: {
      draw: [(u) => { drawBridges(u); drawFailedMarkers(u); }],
    },
  };
  new uPlot(opts, data, container);
}

buildGrid();
</script>
</body>
</html>
"""


CHART_HTML = r"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>liboil chart</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/uplot@1.6.27/dist/uPlot.min.css">
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, sans-serif;
         margin: 1.5rem; background: #fafafa; color: #222; }
  .back { font-size: 0.9rem; }
  h1 { margin: 0.5rem 0 0.25rem 0; font-size: 1.3rem; }
  .hint { color: #888; font-size: 0.8rem; margin-top: 0.25rem; }

  #pinned-bar { display: none;
                padding: 0.5rem 0.6rem; margin: 0.5rem 0;
                background: #fff8dc; border: 1px solid #e0c96a;
                border-radius: 3px; font-size: 0.9rem;
                user-select: text; }
  #pinned-bar.show { display: block; }
  #pinned-bar .head { display: flex; align-items: center; gap: 0.5rem;
                      flex-wrap: wrap; }
  #pinned-bar .subject { margin-top: 0.3rem; font-weight: 500; }
  #pinned-bar .subject:empty { display: none; }
  #pinned-bar .meta-line { display: flex; align-items: center; gap: 0.6rem;
                           flex-wrap: wrap; margin-top: 0.25rem;
                           font-size: 0.85rem; color: #555; }
  #pinned-bar .author:empty { display: none; }
  #pinned-bar .author::before { content: "by "; color: #888; }
  #pinned-bar code { font-family: ui-monospace, Menlo, Consolas, monospace;
                     background: #fff; padding: 1px 5px;
                     border-radius: 2px; border: 1px solid #e0c96a; }
  #pinned-bar a.sha { text-decoration: none; color: inherit; }
  #pinned-bar a.sha:hover code { background: #fff3b0; }
  #pinned-bar button { font-size: 0.8rem; cursor: pointer;
                       margin-left: auto; }
  #pinned-stderr-wrap { margin-top: 0.4rem; }
  #pinned-stderr-wrap[hidden] { display: none; }
  #pinned-stderr-wrap summary { cursor: pointer; color: #a00;
                                font-size: 0.85rem; }
  #pinned-stderr { font-family: ui-monospace, Menlo, Consolas, monospace;
                   font-size: 0.8rem; background: #fff;
                   border: 1px solid #e0c96a; border-radius: 2px;
                   padding: 6px 8px; margin: 0.3rem 0 0 0;
                   max-height: 300px; overflow: auto;
                   white-space: pre-wrap; }

  #legend { display: flex; gap: 1.25rem; flex-wrap: wrap;
            margin-top: 0.6rem; font-size: 0.9rem; }
  #legend label { display: inline-flex; align-items: center;
                  gap: 0.35rem; cursor: pointer; user-select: none; }
  #legend .swatch { display: inline-block; width: 14px; height: 14px;
                    border-radius: 2px;
                    border: 1px solid rgba(0,0,0,0.15); }

  #chart { position: relative; margin-top: 0.5rem; }
  .tooltip { position: absolute; top: 8px;
             background: rgba(30,30,30,0.92); color: #eee;
             padding: 6px 10px; font-size: 12px; border-radius: 3px;
             pointer-events: none; white-space: nowrap; z-index: 10;
             box-shadow: 0 2px 6px rgba(0,0,0,0.25); }
  .tooltip code { color: #fc0;
                  font-family: ui-monospace, Menlo, Consolas, monospace; }
  .tooltip .b { display: inline-block; min-width: 55px; }
  .tooltip .pin { background: #fc0; color: #111; padding: 0 4px;
                  border-radius: 2px; font-weight: bold; font-size: 10px;
                  margin-left: 6px; }
  .tooltip .subj { max-width: 380px; white-space: normal;
                   display: inline-block; }
  .tooltip .auth { color: #bbb; font-style: italic; }
  .tooltip .fail { color: #f77; font-style: italic; }
  #pinned-bar.failed { background: #ffe8e8; border-color: #d66; }
  #pinned-bar .reason { color: #a00; font-style: italic;
                        margin-left: 0.5rem; }
  .error { color: #a00; }
</style>
</head>
<body>
<p class="back"><a href="interactive.html">&larr; back to gallery</a></p>
<h1 id="title">liboil chart</h1>

<div id="pinned-bar">
  <div class="head">
    <b id="pinned-date"></b>
    <a id="pinned-sha-link" class="sha" target="_blank" rel="noopener">
      <code id="pinned-sha"></code>
    </a>
    <button id="unpin-btn" type="button">unpin</button>
  </div>
  <div class="subject" id="pinned-subject"></div>
  <div class="meta-line">
    <span class="author" id="pinned-author"></span>
    <span class="reason" id="pinned-reason"></span>
    <span class="times" id="pinned-times"></span>
  </div>
  <details id="pinned-stderr-wrap" hidden>
    <summary>build / run output</summary>
    <pre id="pinned-stderr"></pre>
  </details>
</div>

<div id="chart"></div>
<div id="legend"></div>
<p class="hint">drag to zoom &middot; double-click to reset &middot;
  click a commit to pin it</p>

<script src="data.js"></script>
<script src="https://cdn.jsdelivr.net/npm/uplot@1.6.27/dist/uPlot.iife.min.js"></script>
<script>
__COMMON_JS__

let pinnedIdx = null;
let currentPlot = null;
let currentBackends = null;
let currentTooltip = null;

function main() {
  const params = new URLSearchParams(location.search);
  const cs = params.get("cs");
  const rStr = params.get("r");
  const ratio = rStr === null ? NaN : parseFloat(rStr);
  const chartDiv = document.getElementById("chart");

  if (!cs || !Number.isFinite(ratio)) {
    chartDiv.innerHTML = "<p class='error'>Missing or invalid ?cs= / ?r= "
      + "query parameters.</p>";
    return;
  }

  document.getElementById("title").textContent =
    cs + "  —  scale " + ratio;
  document.title = "liboil — " + cs + " @ " + ratio;

  const g = seriesFor(cs, ratio);
  if (!g) {
    chartDiv.innerHTML = "<p class='error'>No data for "
      + cs + " @ scale " + ratio + ".</p>";
    return;
  }
  const { backends, data } = g;

  const tooltip = document.createElement("div");
  tooltip.className = "tooltip";
  tooltip.style.display = "none";
  chartDiv.appendChild(tooltip);

  const width  = Math.max(600, window.innerWidth - 60);
  const height = Math.max(400, window.innerHeight - 260);

  const opts = {
    width, height,
    cursor: {
      drag: { x: true, y: false },
    },
    legend: { show: false },
    scales: {
      x: {
        // Half-index pad on each side so the leftmost/rightmost points
        // aren't clipped by the plot-area boundary.
        range: (u, dMin, dMax) => [dMin - 0.5, dMax + 0.5],
      },
      y: {
        distr: 3,
        // Fit y to data currently visible (both the x-zoom range and the
        // series the user hasn't hidden in the legend). Called whenever y
        // needs auto-ranging; we also trigger it explicitly on x-zoom via
        // the setScale hook below.
        range: (u, dataMin, dataMax) => {
          const xMin = u.scales.x.min;
          const xMax = u.scales.x.max;
          const lo0 = Math.max(0, Math.ceil(xMin));
          const hi0 = Math.floor(xMax);
          let lo = Infinity, hi = -Infinity;
          for (let s = 1; s < u.series.length; s++) {
            if (!u.series[s].show) continue;
            const arr = u.data[s];
            const end = Math.min(arr.length - 1, hi0);
            for (let i = lo0; i <= end; i++) {
              const v = arr[i];
              if (v == null || !Number.isFinite(v) || v <= 0) continue;
              if (v < lo) lo = v;
              if (v > hi) hi = v;
            }
          }
          if (!Number.isFinite(lo) || !Number.isFinite(hi)) {
            return [dataMin, dataMax];
          }
          // 5% pad in log space.
          const logLo = Math.log10(lo), logHi = Math.log10(hi);
          const pad = Math.max((logHi - logLo) * 0.05, 0.02);
          return [Math.pow(10, logLo - pad), Math.pow(10, logHi + pad)];
        },
      },
    },
    axes: [
      {
        splits: (u, axisIdx, scaleMin, scaleMax) => {
          // Uniform ticks by commit index so visual spacing is always
          // constant — never two ticks one pixel apart when commits happen
          // to cross a month boundary.
          const lo = Math.max(0, Math.ceil(scaleMin));
          const hi = Math.min(DATA.commits.length - 1, Math.floor(scaleMax));
          if (hi < lo) return null;
          const span = hi - lo + 1;
          const target = 10;
          const stride = Math.max(1, Math.floor(span / target));
          const picks = [];
          for (let i = lo; i <= hi; i += stride) picks.push(i);
          if (picks[picks.length - 1] !== hi) picks.push(hi);
          return picks;
        },
        values: (u, splits) => splits.map(v => {
          if (v == null || !Number.isFinite(v)) return "";
          const i = Math.round(v);
          if (i < 0 || i >= DATA.commits.length) return "";
          return DATA.commits[i].date.slice(0, 10);
        }),
      },
      {
        size: 70,
        values: (u, splits) => splits.map(v => {
          if (v == null || !Number.isFinite(v)) return "";
          return v + " ms";
        }),
      },
    ],
    series: [
      { label: "commit" },
      ...backends.map(b => ({
        label: b,
        stroke: BACKEND_COLORS[b] || "#888",
        width: 1.5,
        points: { size: 4 },
      })),
    ],
    hooks: {
      setCursor: [(u) => updateTooltip(u, backends, tooltip)],
      setScale:  [(u, key) => {
        // X-zoom doesn't re-auto-range y by default. Kick it manually so
        // the y range function runs against the newly-visible x window.
        if (key === "x") u.setScale("y", { min: null, max: null });
      }],
      draw:      [(u) => { drawBridges(u); drawFailedMarkers(u); drawPin(u, backends); }],
      ready:     [(u) => wireClick(u, backends, tooltip)],
    },
  };

  currentPlot = new uPlot(opts, data, chartDiv);
  currentBackends = backends;
  currentTooltip = tooltip;
  buildLegend(currentPlot, backends,
              document.getElementById("legend"));

  document.getElementById("unpin-btn").addEventListener("click", () => {
    pinnedIdx = null;
    syncPinnedBar(currentBackends, currentPlot);
    if (currentPlot) currentPlot.redraw();
    if (currentPlot && currentTooltip) {
      updateTooltip(currentPlot, currentBackends, currentTooltip);
    }
  });
}

const COMMIT_URL = "https://github.com/ender672/liboil/commit/";

function buildLegend(u, backends, container) {
  container.textContent = "";
  for (let b = 0; b < backends.length; b++) {
    const seriesIdx = b + 1;
    const label = document.createElement("label");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = true;
    cb.addEventListener("change", () => {
      u.setSeries(seriesIdx, { show: cb.checked });
    });
    const swatch = document.createElement("span");
    swatch.className = "swatch";
    swatch.style.background = BACKEND_COLORS[backends[b]] || "#888";
    const text = document.createElement("span");
    text.textContent = backends[b];
    label.appendChild(cb);
    label.appendChild(swatch);
    label.appendChild(text);
    container.appendChild(label);
  }
}

function wireClick(u, backends, tip) {
  // Track drag so a zoom isn't mistaken for a click.
  let downX = null;
  u.over.addEventListener("mousedown", (e) => { downX = e.clientX; });
  u.over.addEventListener("mouseup", (e) => {
    const dx = downX == null ? 0 : Math.abs(e.clientX - downX);
    downX = null;
    if (dx > 3) return; // drag — leave for zoom
    const idx = u.cursor.idx;
    if (idx == null || idx < 0) return;
    pinnedIdx = (pinnedIdx === idx) ? null : idx;
    u.redraw();
    syncPinnedBar(backends, u);
    updateTooltip(u, backends, tip);
  });
}

function drawPin(u, backends) {
  if (pinnedIdx == null) return;
  const ctx = u.ctx;
  const x = u.valToPos(pinnedIdx, "x", true);
  ctx.save();
  ctx.strokeStyle = "rgba(0,0,0,0.55)";
  ctx.lineWidth = 1;
  ctx.setLineDash([4, 3]);
  ctx.beginPath();
  ctx.moveTo(x, u.bbox.top);
  ctx.lineTo(x, u.bbox.top + u.bbox.height);
  ctx.stroke();
  ctx.setLineDash([]);
  for (let b = 0; b < backends.length; b++) {
    const i = b + 1;
    const v = u.data[i][pinnedIdx];
    if (v == null) continue;
    const y = u.valToPos(v, u.series[i].scale || "y", true);
    ctx.fillStyle   = BACKEND_COLORS[backends[b]] || "#888";
    ctx.strokeStyle = "#fff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(x, y, 5, 0, 2 * Math.PI);
    ctx.fill();
    ctx.stroke();
  }
  ctx.restore();
}

function syncPinnedBar(backends, u) {
  const bar = document.getElementById("pinned-bar");
  if (pinnedIdx == null) {
    bar.classList.remove("show");
    bar.classList.remove("failed");
    return;
  }
  const c = DATA.commits[pinnedIdx];
  if (!c) {
    bar.classList.remove("show");
    bar.classList.remove("failed");
    return;
  }
  document.getElementById("pinned-date").textContent =
    c.date.replace("T", " ").slice(0, 19);
  document.getElementById("pinned-sha").textContent = c.sha;
  document.getElementById("pinned-sha-link").href = COMMIT_URL + c.sha;
  document.getElementById("pinned-subject").textContent = c.subject || "";
  document.getElementById("pinned-author").textContent  = c.author  || "";
  const reasonEl = document.getElementById("pinned-reason");
  if (c.reason) {
    reasonEl.textContent = c.reason;
    bar.classList.add("failed");
  } else {
    reasonEl.textContent = "";
    bar.classList.remove("failed");
  }
  const stderrWrap = document.getElementById("pinned-stderr-wrap");
  const stderrPre  = document.getElementById("pinned-stderr");
  if (c.stderr) {
    stderrPre.textContent = c.stderr;
    stderrWrap.hidden = false;
    stderrWrap.open = false;
  } else {
    stderrPre.textContent = "";
    stderrWrap.hidden = true;
  }
  const times = [];
  if (u && !c.reason) {
    for (let i = 0; i < backends.length; i++) {
      const v = u.data[i + 1][pinnedIdx];
      if (v != null) times.push(backends[i] + " " + v.toFixed(2) + "ms");
    }
  }
  document.getElementById("pinned-times").textContent =
    times.length ? "— " + times.join(", ") : "";
  bar.classList.add("show");
}

function updateTooltip(u, backends, tip) {
  const { left, top, idx } = u.cursor;
  const hovering = (idx != null && left >= 0 && top >= 0);
  const showIdx = hovering ? idx : pinnedIdx;
  if (showIdx == null) {
    tip.style.display = "none";
    return;
  }
  const commit = DATA.commits[showIdx];
  if (!commit) { tip.style.display = "none"; return; }

  const rows = [
    "<b>" + commit.date.replace("T", " ").slice(0, 19) + "</b>",
    "<code>" + commit.sha.slice(0, 12) + "</code>"
      + (showIdx === pinnedIdx ? " <span class='pin'>pinned</span>" : ""),
  ];
  if (commit.subject) {
    rows.push("<span class='subj'>" + escHtml(commit.subject) + "</span>");
  }
  if (commit.author) {
    rows.push("<span class='auth'>" + escHtml(commit.author) + "</span>");
  }
  if (commit.reason) {
    rows.push("<span class='fail'>" + escHtml(commit.reason) + "</span>");
  } else {
    for (let i = 0; i < backends.length; i++) {
      const v = u.data[i + 1][showIdx];
      if (v != null) {
        rows.push("<span class='b'>" + backends[i] + "</span>"
                  + v.toFixed(3) + " ms");
      }
    }
  }
  tip.innerHTML = rows.join("<br>");

  const xPx = hovering ? left : u.valToPos(showIdx, "x");
  const containerW = tip.parentElement.clientWidth;
  if (xPx < containerW / 2) {
    tip.style.left  = "auto";
    tip.style.right = "10px";
  } else {
    tip.style.right = "auto";
    tip.style.left  = "10px";
  }
  tip.style.display = "block";
}

main();
</script>
</body>
</html>
"""


def write_interactive(outdir, grouped, ordered_commits, failures, meta):
    commit_to_idx = {key: i for i, key in enumerate(ordered_commits)}
    n = len(ordered_commits)

    series = {}
    for (cs, ratio), by_backend in grouped.items():
        key = f"{cs}|{ratio:g}"
        series[key] = {}
        for backend, pts in by_backend.items():
            arr = [None] * n
            for d, sha, t in pts:
                arr[commit_to_idx[(d, sha)]] = t
            series[key][backend] = arr

    ratios = sorted({r for (_, r) in grouped})
    color_spaces = sorted({cs for (cs, _) in grouped})

    commits_out = []
    for d, sha in ordered_commits:
        entry = {"date": d.isoformat(), "sha": sha}
        fail = failures.get((d, sha))
        if fail:
            entry["reason"] = fail["reason"]
            if fail["stderr"]:
                entry["stderr"] = fail["stderr"]
        info = meta.get(sha)
        if info:
            author, subject = info
            entry["author"] = author
            entry["subject"] = subject
        commits_out.append(entry)

    data = {
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "commits": commits_out,
        "ratios": ratios,
        "color_spaces": color_spaces,
        "series": series,
    }

    data_js = ("window.BENCH_DATA = "
               + json.dumps(data, separators=(",", ":"))
               + ";")
    (outdir / "data.js").write_text(data_js)

    common = UPLOT_COMMON_JS
    (outdir / "interactive.html").write_text(
        INTERACTIVE_HTML.replace("__COMMON_JS__", common)
    )
    (outdir / "chart.html").write_text(
        CHART_HTML.replace("__COMMON_JS__", common)
    )
    print(f"wrote {outdir / 'data.js'}  ({len(data_js) // 1024} KB)")
    print(f"wrote {outdir / 'interactive.html'}")
    print(f"wrote {outdir / 'chart.html'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", nargs="?", default="benchmarks.csv",
                    help="input CSV (default: benchmarks.csv)")
    ap.add_argument("-o", "--outdir", default="charts",
                    help="output directory (default: charts)")
    ap.add_argument("--repo", default=os.environ.get("LIBOIL_REPO"),
                    help="git repo for commit metadata lookups "
                         "(default: $LIBOIL_REPO)")
    args = ap.parse_args()

    csv_path = Path(args.csv)
    errors_path = csv_path.with_name("errors.log")
    errors = parse_errors(errors_path)
    grouped, ordered_commits, failures = group(parse_rows(csv_path), errors)

    if args.repo:
        meta = fetch_commit_meta([sha for _d, sha in ordered_commits], Path(args.repo))
    else:
        meta = {}

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    write_interactive(outdir, grouped, ordered_commits, failures, meta)
    n_fail = len(failures)
    n_ok   = len(ordered_commits) - n_fail
    print(f"commits: {n_ok} ok, {n_fail} failed/skipped, "
          f"{len(meta)} with git metadata")


if __name__ == "__main__":
    main()
