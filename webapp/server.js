/*
 * server.js — Hub del Cube Tournament Controller (host)
 * -----------------------------------------------------
 * Fa due cose, sulla STESSA porta (8080):
 *   1) serve la web app (cartella ./public) via HTTP
 *   2) fa da ponte WebSocket tra il CUBO (client ESP32) e il BROWSER
 *      dell'host (la web app). Tutta la logica del bracket vive nel
 *      browser: qui dentro si fa solo da centralino ("relay").
 *
 * Chi parla con chi:
 *   - Il cubo si presenta con  {"type":"hello","cube":"cubo-01",...}
 *   - Il browser si presenta con {"type":"hello","role":"host"}
 *   Da lì in poi: messaggi del cubo  -> inoltrati a tutti i browser;
 *                 messaggi del browser -> inoltrati al cubo.
 *
 * Avvio:  npm install   (una volta sola)   poi   npm start
 * L'IP da mettere nel firmware (HOST_IP) e' l'IP di QUESTO PC sulla
 * rete WiFi (ipconfig / ip a). La porta e' PORT qui sotto.
 */

const http = require('http');
const fs   = require('fs');
const path = require('path');
const WebSocket = require('ws');

const PORT   = 8080;                          // = HOST_PORT nel firmware
const PUBLIC = path.join(__dirname, 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css' : 'text/css; charset=utf-8',
  '.js'  : 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg' : 'image/svg+xml',
  '.ico' : 'image/x-icon',
};

// --------------------------- HTTP -------------------------------
const server = http.createServer((req, res) => {
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/') urlPath = '/index.html';
  // niente uscite dalla cartella public
  const safe = path.normalize(urlPath).replace(/^(\.\.[\/\\])+/, '');
  const filePath = path.join(PUBLIC, safe);
  fs.readFile(filePath, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); res.end('404'); return; }
    const ext = path.extname(filePath).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
    res.end(data);
  });
});

// ------------------------- WebSocket ----------------------------
const wss = new WebSocket.Server({ server });
let cube = null;          // socket del cubo (uno solo per ora)
let cubeInfo = null;      // ultimo hello del cubo (cube, table, nfc, mode)
const uis = new Set();    // socket dei browser host

function sendJSON(sock, obj) {
  if (sock && sock.readyState === WebSocket.OPEN) sock.send(JSON.stringify(obj));
}
function broadcastUI(obj) { for (const u of uis) sendJSON(u, obj); }

wss.on('connection', (sock) => {
  sock.role = 'unknown';

  sock.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(raw.toString()); } catch { return; }   // ignora rumore

    // L'hello decide chi e' il mittente
    if (msg.type === 'hello') {
      if (msg.cube) {                                  // -> e' il CUBO
        sock.role = 'cube';
        cube = sock;
        cubeInfo = { cube: msg.cube, table: msg.table, nfc: msg.nfc, mode: msg.mode };
        console.log(`[HUB] cubo connesso: ${msg.cube} (tavolo ${msg.table}, modo ${msg.mode})`);
        broadcastUI({ type: 'cube_status', online: true, ...cubeInfo });
        broadcastUI(msg);                              // gira anche l'hello grezzo
        return;
      }
      if (msg.role === 'host') {                       // -> e' un BROWSER
        sock.role = 'ui';
        uis.add(sock);
        console.log('[HUB] web app host connessa');
        sendJSON(sock, { type: 'cube_status',
                         online: !!(cube && cube.readyState === WebSocket.OPEN),
                         ...(cubeInfo || {}) });
        return;
      }
    }

    if (sock.role === 'cube') { broadcastUI(msg); return; }  // cubo -> tutti gli host
    if (sock.role === 'ui')   { sendJSON(cube, msg);          // host -> cubo
                                if (!cube) console.log('[HUB] comando host ma nessun cubo connesso:', msg.type); }
  });

  sock.on('close', () => {
    if (sock.role === 'cube') {
      cube = null; cubeInfo = null;
      console.log('[HUB] cubo disconnesso');
      broadcastUI({ type: 'cube_status', online: false });
    }
    if (sock.role === 'ui') uis.delete(sock);
  });
});

server.listen(PORT, () => {
  console.log('========================================================');
  console.log(` Cube Tournament host pronto su http://localhost:${PORT}`);
  console.log(' Apri quell\'indirizzo nel browser del PC host.');
  console.log(' Nel firmware metti HOST_IP = IP di questo PC sulla rete.');
  console.log('========================================================');
});
