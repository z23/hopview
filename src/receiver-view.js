"use strict";
const byId = id => document.getElementById(id);
const recordings = new Map();
const recording = byId("recording");
const statusLine = byId("status");
const utcInput = ms => new Date(ms).toISOString().slice(0, -1);
const timeValue = field => Date.parse(byId(field).value + "Z");
let revision = 0;

async function read(path) {
    const response = await fetch(path);
    if (!response.ok) throw new Error(await response.text());
    return response.json();
}

function plot(buckets) {
    const canvas = byId("plot");
    const ctx = canvas.getContext("2d");
    const peak = Math.max(1, ...buckets.map(b => b.avg));
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    for (const b of buckets) {
        const x = b.x;
        if (b.loss > 0) {
            ctx.fillStyle = "#d88a74";
            const height = b.loss * 1.6;
            ctx.fillRect(x, canvas.height - height, 2, height);
        }
        if (b.avg >= 0) {
            ctx.fillStyle = "#7be3d0";
            ctx.fillRect(x, (canvas.height - 3) * (1 - b.avg / peak), 3, 3);
        }
    }
    byId("legend").textContent = "Teal dots show mean RTT (0–" + peak.toFixed(1) +
        " ms). Coral bars show loss (0–100%). Blank spans have no samples.";
}

async function inspect() {
    const request = ++revision;
    const session = recording.value, from = timeValue("from"), through = timeValue("through");
    if (!session || !Number.isFinite(from) || !Number.isFinite(through)) return;
    const params = new URLSearchParams({session, from, through});
    statusLine.textContent = "Reading interval…";
    byId("trace").hidden = true;
    try {
        const view = await read("/api/view?" + params);
        if (request !== revision) return;
        history.replaceState({}, "", "/?" + params);
        const table = byId("hops");
        table.replaceChildren();
        const metric = n => typeof n === "number" && n >= 0 ? n.toFixed(1) : "—";
        const m = view.measurement || {};
        const sessionInfo = recordings.get(session);
        const measurement = [m.resolvedAddress, m.effectiveProtocol?.toUpperCase(), m.effectiveInterface];
        if (m.effectivePort) measurement.push("port " + m.effectivePort);
        if (sessionInfo.dscp) measurement.push("DSCP " + sessionInfo.dscp);
        if (m.effectiveProtocol !== "tcp" && sessionInfo.payloadBytes)
            measurement.push(sessionInfo.payloadBytes + " byte payload");
        if (sessionInfo.dontFragment) measurement.push("DF");
        if (view.resolution) measurement.push(view.resolution);
        byId("measurement").textContent = measurement.filter(Boolean).join(" · ");
        byId("impact").textContent = view.impact?.description || "";
        for (const hop of view.rows) {
            const tr = table.insertRow();
            for (const value of [hop.hop, hop.address || "—", hop.sent, metric(hop.loss),
                                 metric(hop.min), metric(hop.avg), metric(hop.max), hop.lastFailure?.kind || "—"])
                tr.insertCell().textContent = value;
        }
        plot(view.charts[0]?.buckets || []);
        statusLine.textContent = view.rows.length ? new Date(from).toISOString() + " – " +
            new Date(through).toISOString() : "No samples in this interval.";
        byId("trace").hidden = false;
    } catch (error) { if (request === revision) statusLine.textContent = error.message; }
}
function selectWindow(from, through) {
    byId("from").value = utcInput(from);
    byId("through").value = utcInput(through);
    return inspect();
}
async function load(latest) {
    try {
        const sessions = await read("/api/sessions");
        const params = new URLSearchParams(location.search);
        const preferred = latest ? recording.value : params.get("session");
        recording.replaceChildren();
        recordings.clear();
        for (const s of sessions) {
            recordings.set(String(s.id), s);
            recording.add(new Option(s.location + " · " + s.target + " (" + s.resolvedAddress + ")", s.id));
        }
        if (!sessions.length) { statusLine.textContent = "Waiting for a collector recording."; return; }
        if (preferred && !recordings.has(preferred)) { statusLine.textContent = "Recording unavailable."; return; }
        if (preferred) recording.value = preferred;
        const end = recordings.get(recording.value).endMs;
        const requested = !latest && params.has("from") && params.has("through");
        await selectWindow(requested ? Number(params.get("from")) : Math.max(0, end - 900000),
                           requested ? Number(params.get("through")) : end);
    } catch (error) { statusLine.textContent = error.message; }
}
byId("window").addEventListener("submit", event => { event.preventDefault(); inspect(); });
byId("latest").addEventListener("click", () => load(true));
recording.addEventListener("change", () => {
    const end = recordings.get(recording.value).endMs;
    selectWindow(Math.max(0, end - 900000), end);
});
load(false);
