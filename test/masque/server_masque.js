// Local ttsignal QUIC server for the MASQUE end-to-end test. Echoes any data
// frame back to the client. Listens on 127.0.0.1:4434 (ALPN ttsignal).
process.ttsBuildType = 'debug';
const _m = require('ttsignal');
const ttsignal = _m.ttsignal || _m;
const path = require('path');

const CERT_DIR = path.join(__dirname, '..', '..', 'certs');

function ts() { return new Date().toISOString(); }
function log(...a) { console.log(ts(), ...a); }

const server = ttsignal.createServer({
  alpn: 'ttsignal',
  host: '127.0.0.1',
  port: 4434,
  server_host: '127.0.0.1',
  server_port: 4434,
  idle_time_out: 20000,
  log_level: 'info',
  log_file: path.join(__dirname, 'ttserver.log'),
  private_key_file: path.join(CERT_DIR, 'localhost.key'),
  cert_file: path.join(CERT_DIR, 'localhost.crt'),
  ping_on: true,
  ping_interval: 5000,
});
log('server created');

server.on('connection', (conn) => {
  log('new connection');
  conn.on('error', (err) => log('conn error', err));
  conn.on('close', (had_error) => log('conn closed', had_error));
  conn.on('connect', (props) => {
    log('connect', props);
    conn.accept(true, { server_param: 'ok' });
  });
  conn.on('data', (data) => {
    log('recv data', JSON.stringify(data.toString('utf8')));
    conn.sendData(Buffer.from('echo: ' + data.toString('utf8')));
  });
});
server.start();
log('server started on 127.0.0.1:4434');
