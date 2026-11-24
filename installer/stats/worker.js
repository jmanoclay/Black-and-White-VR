// Black & White VR launch counter: a Cloudflare Worker with a D1 database bound as DB and a secret
// STATS_KEY (Settings → Variables and Secrets). Paste this whole file into the Worker's editor.
//
// POST /launch   {"id": "<32 hex>", "version": "0.3.1", "setup": "1.0.14"}   → 204
//   Sent by the launcher on Launch in VR (when the player has it on). The id is random, made by the
//   launcher on first use; nothing about the player or their computer is sent, and the IP address
//   (which every web request carries) is not stored. One row per id, version and day: a count, capped.
// GET /stats     with the header  Authorization: Bearer <STATS_KEY>               → JSON
//   Players and launches per day (30 days), new players per day, players per version (7 days), totals.

const ID = /^[0-9a-f]{32}$/;
const VER = /^[0-9A-Za-z.\-]{1,20}$/;
let ready = false;

async function init(db) {
  if (ready) return;
  await db.prepare(
    "CREATE TABLE IF NOT EXISTS launches (day TEXT NOT NULL, id TEXT NOT NULL, version TEXT NOT NULL, " +
    "setup TEXT NOT NULL DEFAULT '', count INTEGER NOT NULL DEFAULT 1, PRIMARY KEY (day, id, version))").run();
  ready = true;
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    await init(env.DB);

    if (req.method === "POST" && url.pathname === "/launch") {
      let b;
      try { b = await req.json(); } catch { return new Response("bad json", { status: 400 }); }
      const id = String(b.id || ""), version = String(b.version || ""), setup = String(b.setup || "");
      if (!ID.test(id) || !VER.test(version) || (setup && !VER.test(setup)))
        return new Response("bad ping", { status: 400 });
      const day = new Date().toISOString().slice(0, 10);
      await env.DB.prepare(
        "INSERT INTO launches (day, id, version, setup) VALUES (?, ?, ?, ?) " +
        "ON CONFLICT (day, id, version) DO UPDATE SET count = MIN(count + 1, 100), setup = excluded.setup")
        .bind(day, id, version, setup).run();
      return new Response(null, { status: 204 });
    }

    if (req.method === "GET" && url.pathname === "/stats") {
      if (!env.STATS_KEY || req.headers.get("Authorization") !== "Bearer " + env.STATS_KEY)
        return new Response("not allowed", { status: 401 });
      const q = (sql) => env.DB.prepare(sql).all().then((r) => r.results);
      const [total, days, fresh, versions] = await Promise.all([
        q("SELECT COUNT(DISTINCT id) AS players, SUM(count) AS launches, MIN(day) AS since FROM launches"),
        q("SELECT day, COUNT(DISTINCT id) AS players, SUM(count) AS launches FROM launches " +
          "WHERE day >= date('now', '-30 days') GROUP BY day ORDER BY day"),
        q("SELECT first AS day, COUNT(*) AS new_players FROM (SELECT id, MIN(day) AS first FROM launches GROUP BY id) " +
          "WHERE first >= date('now', '-30 days') GROUP BY first ORDER BY first"),
        q("SELECT version, COUNT(DISTINCT id) AS players FROM launches " +
          "WHERE day >= date('now', '-7 days') GROUP BY version ORDER BY version DESC"),
      ]);
      return Response.json({ total: total[0], days, new_players: fresh, versions_7d: versions });
    }

    return new Response("Black & White VR launch counter\n", { headers: { "Content-Type": "text/plain" } });
  },
};
