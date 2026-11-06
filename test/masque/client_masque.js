// End-to-end MASQUE CONNECT-UDP test client for ttsignal.
//
//   server.js (ttsignal QUIC server, ALPN ttsignal)   127.0.0.1:4434
//   masque_proxy.py (aioquic CONNECT-UDP proxy, h3)    127.0.0.1:4435
//   this client: inner QUIC -> proxy -> server
//
// Run with DIRECT=1 to bypass the proxy (baseline), otherwise it tunnels
// through the MASQUE proxy. Exits non-zero on failure / timeout.

process.ttsBuildType = 'debug';
const _m = require('ttsignal');
const tts = _m.ttsignal || _m;
const fs = require('fs');
const path = require('path');

const DIRECT = !!process.env.DIRECT;
const CA = fs.readFileSync(path.join(__dirname, '..', '..', 'certs', 'localhost.crt'), 'utf8');
const SERVER_URL = 'https://127.0.0.1:4434/rtc';
const PROXY_URL = 'masque://127.0.0.1:4435';

function ts() {
  return new Date().toISOString();
}
function log(...a) { console.log(ts(), ...a); }

const connectorCfg = {
  alpn: 'ttsignal',
  ca_cert_pem: CA,
  idle_time_out: 20000,
  log_level: tts.LOG_LEVEL_DEBUG,
  log_file: path.join(__dirname, 'ttclient.log'),
  taskThreads: 1,
};
if (!DIRECT) {
  connectorCfg.proxyUrl = PROXY_URL;
  // The self-signed cert only carries a DNS SAN (localhost); ttsignal's cert
  // verify matches the SNI against DNS SANs, so present SNI=localhost while
  // still dialing 127.0.0.1.
  connectorCfg.proxySni = 'localhost';
}

log(DIRECT ? '=== BASELINE (direct) ===' : '=== MASQUE proxy ===', connectorCfg.proxyUrl || '');

const connector = tts.createConnector(connectorCfg);
const conn = connector.createConnection({
  alpn: 'ttsignal',
  server_host: 'localhost',
  active_connection_id_limit: 1,
});

let done = false;
function finish(code, msg) {
  if (done) return;
  done = true;
  log(msg);
  try { conn.close(); } catch (e) {}
  setTimeout(() => process.exit(code), 300);
}

const failTimer = setTimeout(() => finish(1, 'TIMEOUT: no echo within 12s'), 12000);

conn.on('error', (err) => log('conn error', err));
conn.on('close', (had_error) => log('conn close', had_error));

conn.connect(SERVER_URL, { Authorization: 'test' }, 10000, (err, resp) => {
  if (err) {
    clearTimeout(failTimer);
    return finish(1, 'CONNECT FAILED: ' + err);
  }
  log('CONNECTED', resp);
  conn.on('streamCreated', (stream) => {
    log('streamCreated');
    stream.on('data', (data) => {
      log('RECV', JSON.stringify(data.toString('utf8')));
      clearTimeout(failTimer);
      finish(0, 'SUCCESS: round-trip through ' + (DIRECT ? 'direct' : 'MASQUE proxy') + ' works');
    });
    stream.sendData(Buffer.from('ping from masque test client'));
    log('sent ping');
  });
});
