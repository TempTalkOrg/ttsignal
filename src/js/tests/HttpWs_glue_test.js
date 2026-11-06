#!/usr/bin/env node
'use strict';

/*******************************************************************************
 * src/js/index.js 里 HTTP / WebSocket 胶水层的端到端测试。
 *
 *   node src/js/tests/HttpWs_glue_test.js
 *
 * ⚠️ 测的是**胶水层**（Promise / EventEmitter 形态），不是原生绑定 —— 原生绑定
 * 自己那两套用例在 src/cpp/napi/tests/ 下。这里刻意**直接 require 仓库里的
 * src/js/index.js**（而不是 node_modules/ttsignal 那份拷贝），免得测到一份过期
 * 副本、改了代码却是假绿。原生产物仍然是 cmake 出来的那个正式 .node：
 * 前面有一轮的测试因为手搓编译命令少了 -DTT_HAS_PATH_MONITOR，网卡绑定整段被
 * 条件编译掉、缺陷被构建配置完整屏蔽，所以用例 A 会在运行期再核一遍
 * hasPathMonitor。
 *
 * 公网用例依赖本机的企业 VPN：默认路由走 utun，物理网卡 en0 直连。
 * force-physical 拿到的公网出口 IP 必须与 os 档不同 —— 这是"胶水层把真实 IP
 * 能力完整带上来了"的最硬证据。VPN 没连时这条会被跳过而不是判失败。
 *
 * 本地 wss 用 certs/localhost.{crt,key}（该目录被 .gitignore），缺了就跳过。
 * 生成方式：
 *   openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
 *       -keyout certs/localhost.key -out certs/localhost.crt \
 *       -subj /CN=localhost -addext subjectAltName=DNS:localhost
 *
 * 退出码：0 = 全过（含跳过），1 = 有失败。
 ******************************************************************************/

const assert = require('assert');
const child_process = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const https = require('https');
const net = require('net');
const path = require('path');

///////////////////////////////////////////////////////////////////////////////
// 加载胶水层（src/js/index.js）+ 正式原生产物
///////////////////////////////////////////////////////////////////////////////

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const GLUE_PATH = path.join(REPO_ROOT, 'src', 'js', 'index.js');
const ADDON_PATH = process.env.TTSIGNAL_NATIVE_PATH || path.join(
    REPO_ROOT, 'node_modules', 'ttsignal', 'dist', 'build', 'Debug',
    `ttsignal.${process.platform}.${process.arch}.node`);

if (!fs.existsSync(ADDON_PATH)) {
    console.error(`找不到原生产物：${ADDON_PATH}`);
    console.error('先跑 build/macos-arm64-debug/build（或对应平台的构建脚本）。');
    process.exit(1);
}
// src/js/index.js 的 resolveAddonPath() 会先看这个环境变量。src/js/ 下面没有
// build/ 目录，所以不设它就找不到 .node。
process.env.TTSIGNAL_NATIVE_PATH = ADDON_PATH;

const tts = require(GLUE_PATH);

///////////////////////////////////////////////////////////////////////////////
// 小工具
///////////////////////////////////////////////////////////////////////////////

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 任何一条用例卡住都要能看出来，而不是让整个脚本挂死。
function withTimeout(promise, ms, label) {
    let timer;
    return Promise.race([
        promise.finally(() => clearTimeout(timer)),
        new Promise((_, reject) => {
            timer = setTimeout(
                () => reject(new Error(`[${label}] 等待 ${ms}ms 仍未 settle`)), ms);
        })
    ]);
}

// 轮询等一个条件成立（事件是异步经 uv_async 交付的，没法同步断言）
async function waitFor(pred, ms, label) {
    const deadline = Date.now() + ms;
    for (;;) {
        if (pred()) return;
        if (Date.now() > deadline) {
            throw new Error(`[${label}] 等待 ${ms}ms 条件仍未成立`);
        }
        await sleep(20);
    }
}

// 断言一个 Promise 在 ms 内 resolve（用来钉住"立刻 resolve"这类语义）
async function resolvesWithin(promise, ms, label) {
    const started = Date.now();
    await withTimeout(promise, ms, label);
    return Date.now() - started;
}

///////////////////////////////////////////////////////////////////////////////
// 本地服务端：明文 http、黑洞、wss（手写握手 + 帧编解码）
///////////////////////////////////////////////////////////////////////////////

const TLS_CERT = path.join(REPO_ROOT, 'certs', 'localhost.crt');
const TLS_KEY = path.join(REPO_ROOT, 'certs', 'localhost.key');
const hasTlsCert = fs.existsSync(TLS_CERT) && fs.existsSync(TLS_KEY);
const CA_PEM = hasTlsCert ? fs.readFileSync(TLS_CERT, 'utf8') : '';
const NO_TLS_CERT_HINT =
    '本机没有 certs/localhost.{crt,key}（该目录被 .gitignore），生成方式见本文件顶部';

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OP_TEXT = 0x1, OP_BINARY = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xa;
const servers = {};

function encodeFrame(opcode, payload) {
    const len = payload.length;
    let header;
    if (len < 126) {
        header = Buffer.alloc(2);
        header[1] = len;
    } else if (len < 65536) {
        header = Buffer.alloc(4);
        header[1] = 126;
        header.writeUInt16BE(len, 2);
    } else {
        header = Buffer.alloc(10);
        header[1] = 127;
        header.writeUInt32BE(0, 2);
        header.writeUInt32BE(len, 6);
    }
    header[0] = 0x80 | opcode;      // 服务端发出的帧不掩码（RFC 6455 5.1）
    return Buffer.concat([header, payload]);
}

// 服务端侧解帧。客户端发的都是单帧不分片，这里不做分片重组。
function makeDecoder(onFrame) {
    let buf = Buffer.alloc(0);
    return function (chunk) {
        buf = Buffer.concat([buf, chunk]);
        for (;;) {
            if (buf.length < 2) return;
            const b0 = buf[0], b1 = buf[1];
            const masked = (b1 & 0x80) !== 0;
            let len = b1 & 0x7f;
            let off = 2;
            if (len === 126) {
                if (buf.length < off + 2) return;
                len = buf.readUInt16BE(off); off += 2;
            } else if (len === 127) {
                if (buf.length < off + 8) return;
                len = buf.readUInt32BE(off + 4); off += 8;
            }
            let mask = null;
            if (masked) {
                if (buf.length < off + 4) return;
                mask = buf.subarray(off, off + 4); off += 4;
            }
            if (buf.length < off + len) return;
            const payload = Buffer.from(buf.subarray(off, off + len));
            if (mask) {
                for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
            }
            buf = buf.subarray(off + len);
            onFrame({ opcode: b0 & 0x0f, masked, payload });
        }
    };
}

function handleUpgrade(req, socket) {
    const p = (req.url || '/').split('?')[0];
    const send = (opcode, payload) =>
        socket.write(encodeFrame(opcode, payload || Buffer.alloc(0)));
    socket.on('error', () => { /* 客户端随时可能 RST */ });
    socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                 'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                 'Sec-WebSocket-Accept: ' + crypto.createHash('sha1')
                     .update(String(req.headers['sec-websocket-key']) + WS_GUID)
                     .digest('base64') + '\r\n' +
                 'X-Test-Server: ttsignal-glue\r\n\r\n');

    socket.on('data', makeDecoder((f) => {
        switch (f.opcode) {
        case OP_TEXT:
            if (p === '/echo') send(OP_TEXT, f.payload);
            if (p === '/burst' && f.payload.toString('utf8') === 'go') {
                // ⚠️ **一次 write 把 6 帧一起送出去**：保证 6 个事件落在同一批
                // uv_async 里 —— 那正是"监听器抛异常会打死进程"的必现形态。
                const frames = [];
                for (let i = 0; i < 6; i++) {
                    frames.push(encodeFrame(OP_TEXT, Buffer.from('burst-' + i)));
                }
                socket.write(Buffer.concat(frames));
            }
            break;
        case OP_BINARY:
            if (p === '/echo') send(OP_BINARY, f.payload);
            break;
        case OP_PING:
            send(OP_PONG, f.payload);
            break;
        case OP_CLOSE:
            send(OP_CLOSE, f.payload);
            socket.end();
            break;
        default:
            break;
        }
    }));
}

function startServers() {
    return new Promise((resolve) => {
        const plain = http.createServer((req, res) => {
            const chunks = [];
            req.on('data', (c) => chunks.push(c));
            req.on('end', () => {
                if (req.url === '/notfound') {
                    res.writeHead(404, 'Not Found', { 'content-type': 'text/plain' });
                    res.end('nope');
                    return;
                }
                if (req.url === '/echo') {
                    res.writeHead(200, { 'content-type': 'application/json' });
                    res.end(JSON.stringify({
                        method: req.method,
                        headers: req.headers,
                        body: Buffer.concat(chunks).toString('utf8')
                    }));
                    return;
                }
                res.writeHead(200, 'OK', { 'content-type': 'text/plain' });
                res.end('hello');
            });
        });
        plain.listen(0, '127.0.0.1', () => {
            servers.plainPort = plain.address().port;
            servers.plain = plain;
            const silent = net.createServer(() => { /* 只 accept，永不应答 */ });
            silent.listen(0, '127.0.0.1', () => {
                servers.silentPort = silent.address().port;
                servers.silent = silent;
                if (!hasTlsCert) { resolve(); return; }
                const wss = https.createServer({
                    cert: fs.readFileSync(TLS_CERT),
                    key: fs.readFileSync(TLS_KEY)
                });
                wss.on('upgrade', handleUpgrade);
                wss.on('request', (req, res) => { res.writeHead(426); res.end(); });
                wss.on('clientError', () => {});
                wss.listen(0, '127.0.0.1', () => {
                    servers.wssPort = wss.address().port;
                    servers.wss = wss;
                    resolve();
                });
            });
        });
    });
}

function stopServers() {
    ['plain', 'silent', 'wss'].forEach((k) => {
        if (servers[k]) servers[k].close();
    });
}

const httpUrl = (p) => `http://127.0.0.1:${servers.plainPort}${p}`;
const wssUrl = (p) => `wss://localhost:${servers.wssPort}${p}`;
const PUBLIC_IP_URL = 'https://ipinfo.io/ip';   // ifconfig.me 从物理网卡连不通

// 本地 wss 用自签证书，一律显式给 caCerts（不用 insecureSkipVerify —— 那会把证书
// 校验整条路绕过去，等于少测一块）
function wsConfig(extra) {
    return Object.assign({ vpnPolicy: 'os', logLevel: 1, caCerts: CA_PEM }, extra);
}

// 把一条连接的事件收进数组，顺带返回它
function collect(conn) {
    const evs = { text: [], data: [], closed: [], exception: [] };
    conn.on('text', (s) => evs.text.push(s));
    conn.on('data', (b) => evs.data.push(b));
    conn.on('closed', (r) => evs.closed.push(r));
    conn.on('exception', (m) => evs.exception.push(m));
    return evs;
}

///////////////////////////////////////////////////////////////////////////////
// 用例框架
///////////////////////////////////////////////////////////////////////////////

const cases = [];
let failed = 0, passed = 0, skipped = 0;
function test(name, fn) { cases.push({ name, fn }); }
class Skip extends Error {}
function skip(reason) { throw new Skip(reason); }
const shared = {};

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

test('A. 胶水层导出形态 / hasPathMonitor 在位 / SMP 那套没被破坏', async () => {
    // require('<glue>') 现在直接就是模块本身（历史形态是 { ttsignal }）
    assert.strictEqual(typeof tts.createHttpConnector, 'function');
    assert.strictEqual(typeof tts.createWsConnector, 'function');
    assert.strictEqual(typeof tts.createConnector, 'function', 'SMP 的 createConnector 丢了');
    assert.strictEqual(typeof tts.createServer, 'function');
    assert.strictEqual(typeof tts.Stream, 'function');
    // 旧写法（js/pathChange.js: const {ttsignal} = require('ttsignal')）仍然可用
    assert.strictEqual(tts.ttsignal, tts, '.ttsignal 自引用没了，旧调用方会拿到 undefined');
    // 自引用不可枚举：console.dir / 遍历导出对象时不该多出一层环
    assert.strictEqual(Object.keys(tts).indexOf('ttsignal'), -1);
    // 错误码常量
    assert.strictEqual(tts.TTS_R_NO_PHYSICAL_INTERFACE, 64);
    assert.strictEqual(tts.TTS_R_ROUTE_MISMATCH, 70);

    // ⚠️ 这条断言是"构建配置悄悄把网卡绑定关掉"这一整类缺陷的运行期防线。
    // 少了 TT_HAS_PATH_MONITOR，prefer-physical 会静默回落系统路由（= VPN 隧道），
    // force-physical 硬失败 64。CMake 的 configure 期断言管不到手搓编译出来的产物。
    const h = tts.createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    const hi = h.info();
    console.log(`      http.info(): hasPathMonitor=${hi.hasPathMonitor} ` +
                `vpnPolicy=${hi.vpnPolicy} drainTimeoutMs=${hi.drainTimeoutMs}`);
    assert.strictEqual(hi.hasPathMonitor, true,
        '产物缺少 TT_HAS_PATH_MONITOR：网卡绑定整段被条件编译掉了');
    await h.close();

    const w = tts.createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    const wi = w.info();
    console.log(`      ws.info():   hasPathMonitor=${wi.hasPathMonitor} ` +
                `vpnPolicy=${wi.vpnPolicy}`);
    assert.strictEqual(wi.hasPathMonitor, true);
    assert.strictEqual(typeof w.stats(), 'object');
    const idle = w.createConnection();
    assert.strictEqual(idle.info().hasPathMonitor, true);
    await w.close();

    // SMP(QUIC) 那套胶水层必须一点没坏：真的建一个 Connector + Connection 出来
    // （只看符号在不在是不够的 —— inherits/原型改坏了要到这一步才露馅）。
    const quic = tts.createConnector({ alpn: 'ttsignal', log_level: 1, taskThreads: 1 });
    assert.strictEqual(typeof quic.on, 'function', 'SMP Connector 不再是 EventEmitter');
    assert.strictEqual(typeof quic.createConnection, 'function');
    const qconn = quic.createConnection({});
    assert.strictEqual(typeof qconn.on, 'function', 'SMP Connection 不再是 EventEmitter');
    assert.strictEqual(typeof qconn.connect, 'function');
    qconn.close();
    quic.close();
    console.log('      SMP(QUIC) 的 Connector / Connection 仍然可建、仍是 EventEmitter');

    // 非法 vpnPolicy 在 JS 层就抛，不静默回落
    assert.throws(() => tts.createHttpConnector({ vpnPolicy: 'force_physical' }),
                  /Invalid vpnPolicy/);
    assert.throws(() => tts.createWsConnector({ vpnPolicy: 42 }), /Invalid vpnPolicy/);
    assert.throws(() => tts.createHttpConnector('nope'), /Invalid config/);
});

test('A2. require("ttsignal") 两种写法都能用（子进程里验真实包入口）', async () => {
    // 这条钉住的是"导出不匹配"那个既有问题：`require('ttsignal').createConnector`
    // 以前是 undefined，js/client.js 之类的示例脚本一直跑不起来。
    // node_modules/ttsignal/index.js 是 src/js/index.js 的拷贝（各平台 build 脚本
    // 里那句 cp），这里先同步一份再验，免得测到过期副本。
    const pkgIndex = path.join(REPO_ROOT, 'node_modules', 'ttsignal', 'index.js');
    if (!fs.existsSync(pkgIndex)) skip('node_modules/ttsignal 不在，跳过包入口验证');
    fs.copyFileSync(GLUE_PATH, pkgIndex);
    const code = [
        "const a = require('ttsignal');",
        "const { ttsignal } = require('ttsignal');",
        "const out = [typeof a.createConnector, typeof a.createHttpConnector,",
        " typeof a.createWsConnector, typeof ttsignal.createConnector,",
        " String(ttsignal === a)];",
        "console.log(out.join(','));",
        // ⚠️ 必须显式退出：加载 ttsignal 之后 JsExchanger 的 uv_async handle 是
        // uv_ref 过的，event loop 不会自己空掉，子进程会永远挂着（既有行为）。
        "process.exit(0);"
    ].join('');
    const r = child_process.spawnSync(process.execPath, ['-e', code], {
        cwd: REPO_ROOT, encoding: 'utf8', timeout: 20000,
        env: Object.assign({}, process.env, { TTSIGNAL_NATIVE_PATH: ADDON_PATH })
    });
    console.log(`      子进程输出：${String(r.stdout).trim()}`);
    if (r.status !== 0) console.log(`      stderr: ${String(r.stderr).slice(0, 300)}`);
    assert.strictEqual(r.status, 0, 'require("ttsignal") 在子进程里就失败了');
    assert.strictEqual(String(r.stdout).trim(),
                       'function,function,function,function,true');
});

test('B. HTTP os 档连公网，记下出口 IP', async () => {
    const c = tts.createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const r = await withTimeout(
            c.request({ url: PUBLIC_IP_URL, timeoutMs: 15000 }), 20000, 'B');
        assert.strictEqual(r.status, 200);
        assert.ok(Buffer.isBuffer(r.body), 'body 应该是 Buffer');
        shared.ipOs = r.body.toString('utf8').trim();
        assert.match(shared.ipOs, /^\d+\.\d+\.\d+\.\d+$/);
        console.log(`      os             -> peerIp=${r.peerIp} ` +
                    `boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod || '(未绑定)'} 出口IP=${shared.ipOs}`);
    } finally {
        await c.close();
    }
});

test('C. HTTP force-physical 连公网：出口 IP 必须与 os 档不同（核心承诺）', async () => {
    const c = tts.createHttpConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    try {
        const r = await withTimeout(
            c.request({ url: PUBLIC_IP_URL, timeoutMs: 15000 }), 20000, 'C');
        assert.strictEqual(r.status, 200);
        const ip = r.body.toString('utf8').trim();
        console.log(`      force-physical -> peerIp=${r.peerIp} ` +
                    `boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod} 出口IP=${ip}`);
        assert.ok(r.boundIfIndex > 0, 'boundIfIndex 为 0 = 根本没绑网卡');
        assert.ok(r.pinMethod.length > 0, 'pinMethod 为空 = 没绑');
        assert.match(ip, /^\d+\.\d+\.\d+\.\d+$/);
        if (!shared.ipOs) skip('用例 B 没拿到 os 档 IP，无法对照');
        if (shared.ipOs === ip) {
            skip(`os 与 force-physical 出口 IP 相同（${ip}）—— 本机此刻大概没连 VPN，` +
                 '这条对照失去意义');
        }
        shared.ipPhysical = ip;
    } finally {
        await c.close();
    }
});

test('D. HTTP force-physical 连环回 -> reject，result=70 且带原生现场描述', async () => {
    const c = tts.createHttpConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    try {
        await assert.rejects(
            withTimeout(c.request({ url: httpUrl('/ok'), timeoutMs: 4000 }), 8000, 'D'),
            (err) => {
                console.log(`      result=${err.result} errName=${err.errName}`);
                console.log(`      errMessage=${err.errMessage}`);
                assert.strictEqual(err.result, tts.TTS_R_ROUTE_MISMATCH,
                    'BC_R_ROUTE_MISMATCH(70) 必须原样透传到 Promise 的 reject');
                assert.strictEqual(err.errName, 'BC_R_ROUTE_MISMATCH');
                // "失败可诊断"是 force-physical 的验收项：这段现场描述只存在于
                // 原生文案里，胶水层按码硬编的通用提示给不出来。硬断言，别只打印。
                assert.ok(typeof err.errMessage === 'string' && err.errMessage.length > 0,
                          'errMessage 丢了');
                assert.match(err.errMessage, /force-physical/);
                assert.match(err.errMessage, /vpnPolicy=os|prefer-physical/);
                assert.match(err.errMessage, /127\.0\.0\.1/, '文案里没有对端 IP');
                assert.match(err.errMessage, /ifIndex=\d+/, '文案里没有网卡编号');
                assert.match(err.errMessage, /peerClass=0x/, '文案里没有 peerClass');
                return true;
            });
    } finally {
        await c.close();
    }
});

test('E. HTTP prefer-physical 连环回 -> resolve（会自动放弃绑定）', async () => {
    const c = tts.createHttpConnector({ vpnPolicy: 'prefer-physical', logLevel: 1 });
    try {
        const r = await withTimeout(
            c.request({ url: httpUrl('/ok'), timeoutMs: 4000 }), 8000, 'E');
        assert.strictEqual(r.status, 200);
        assert.strictEqual(r.reason, 'OK');
        assert.strictEqual(r.body.toString('utf8'), 'hello');
        assert.strictEqual(r.peerIp, '127.0.0.1');
        console.log(`      status=${r.status} boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod || '(已放弃绑定)'}`);
    } finally {
        await c.close();
    }
});

test('F. HTTP Promise 两条路：404/POST 是 resolve，参数错与超时是 reject', async () => {
    const c = tts.createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        // 404 是正常响应
        const r404 = await withTimeout(
            c.request({ url: httpUrl('/notfound'), timeoutMs: 4000 }), 8000, 'F1');
        assert.strictEqual(r404.status, 404);
        assert.strictEqual(r404.reason, 'Not Found');
        assert.strictEqual(r404.headers['content-type'], 'text/plain');
        assert.strictEqual(r404.body.toString('utf8'), 'nope');

        // method / headers / body 完整送达
        const rPost = await withTimeout(c.request({
            method: 'POST', url: httpUrl('/echo'),
            headers: { authorization: 'Bearer t0ken' },
            body: Buffer.from('{"a":1}', 'utf8'), timeoutMs: 4000
        }), 8000, 'F2');
        const echoed = JSON.parse(rPost.body.toString('utf8'));
        assert.strictEqual(echoed.method, 'POST');
        assert.strictEqual(echoed.headers.authorization, 'Bearer t0ken');
        assert.strictEqual(echoed.body, '{"a":1}');

        // 参数用错：胶水层统一转成 reject（不是同步抛）。这类错误没有 result。
        const bad = c.request({});
        assert.ok(bad instanceof Promise, 'request() 必须返回 Promise，不能同步抛');
        await assert.rejects(withTimeout(bad, 5000, 'F3'), (err) => {
            console.log(`      参数错误 -> reject: ${err.message}`);
            assert.match(err.message, /'url' is required/);
            return true;
        });

        // 超时：黑洞对端
        const started = Date.now();
        await assert.rejects(withTimeout(
            c.request({ url: `http://127.0.0.1:${servers.silentPort}/`, timeoutMs: 800 }),
            6000, 'F4'), (err) => {
            const cost = Date.now() - started;
            console.log(`      超时 ${cost}ms result=${err.result} ` +
                        `errName=${err.errName} errMessage=${err.errMessage}`);
            assert.ok(typeof err.result === 'number' && err.result !== 0);
            assert.ok(typeof err.errName === 'string' && err.errName.length > 0);
            assert.ok(cost < 5000, '超时没在 timeoutMs 附近生效');
            return true;
        });
        assert.strictEqual(c.info().pendingRequests, 0);
    } finally {
        await c.close();
    }
});

test('G. HTTP close() 重复调：两个 Promise 都 settle；close 后 request reject', async () => {
    // 原生层把 close 回调收成列表；胶水层每次调都新建一个 Promise。任何一个悬着
    // 就是"await 永远不返回"。
    const c = tts.createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    let closeEvents = 0;
    c.on('close', () => closeEvents++);
    const p1 = c.close();
    const p2 = c.close();
    const settled = [];
    p1.then(() => settled.push(1));
    p2.then(() => settled.push(2));
    await withTimeout(Promise.all([p1, p2]), 8000, 'G');
    await sleep(50);
    console.log(`      两个 close Promise 都 settle：[${settled.join(',')}] ` +
                `close 事件 ${closeEvents} 次`);
    assert.deepStrictEqual(settled.sort(), [1, 2]);
    assert.strictEqual(c.info().closed, true);
    assert.strictEqual(closeEvents, 1, "'close' 事件按 Node 惯例只发一次");
    // 关完之后再 close 也要 settle
    await withTimeout(c.close(), 5000, 'G2');
    await assert.rejects(
        withTimeout(c.request({ url: httpUrl('/ok'), timeoutMs: 1000 }), 5000, 'G3'),
        (err) => {
            console.log(`      close 后 request -> result=${err.result} ` +
                        `errName=${err.errName}`);
            assert.ok(typeof err.result === 'number' && err.result !== 0);
            return true;
        });
});

test('H. WS 事件（text / data / closed）与 connect() 的 Promise', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = tts.createWsConnector(wsConfig());
    try {
        const conn = c.createConnection();
        const evs = collect(conn);
        const ok = await withTimeout(conn.connect(wssUrl('/echo'), 5000), 9000, 'H');
        console.log(`      connect resolve: peerIp=${ok.peerIp} ` +
                    `boundIfIndex=${ok.boundIfIndex} ` +
                    `serverHeader=${ok.headers['X-Test-Server'] ||
                                    ok.headers['x-test-server']}`);
        assert.strictEqual(ok.peerIp, '127.0.0.1');
        assert.strictEqual(ok.boundIfIndex, 0, 'os 档不该绑网卡');
        assert.ok(Object.keys(ok.headers).some((k) => /sec-websocket-accept/i.test(k)));
        assert.strictEqual(conn.info().upgraded, true);

        conn.sendText('hello-ws');
        const bin = Buffer.from([0, 1, 2, 250, 255, 0, 7]);
        conn.sendData(bin);
        await waitFor(() => evs.text.length >= 1 && evs.data.length >= 1, 5000, 'H-echo');
        assert.strictEqual(evs.text[0], 'hello-ws');
        assert.ok(Buffer.isBuffer(evs.data[0]), 'data 事件应该给 Buffer');
        assert.deepStrictEqual(evs.data[0], bin);
        console.log(`      text='${evs.text[0]}' data=${evs.data[0].length}B`);

        // conn.close() 的 Promise：已握手成功的连接等 closed 事件
        const cost = await resolvesWithin(conn.close(), 6000, 'H-close');
        assert.strictEqual(evs.closed.length, 1, 'closed 事件必须恰好一次');
        console.log(`      close() 用了 ${cost}ms，closed reason='${evs.closed[0]}'`);
        assert.strictEqual(conn.info().closed, true);
        // 关掉之后发东西要抛（带 result），不能静默丢
        assert.throws(() => conn.sendText('after-close'), (err) => {
            assert.ok(typeof err.result === 'number' && err.result !== 0,
                      '发送失败的 Error 必须带 result');
            console.log(`      close 后 sendText 抛错：result=${err.result}`);
            return true;
        });
        // 重复 close() 仍然 settle
        await resolvesWithin(conn.close(), 3000, 'H-close2');
    } finally {
        await c.close();
    }
});

test('I. WS force-physical 连环回 -> reject 70 + 现场描述；prefer-physical -> resolve',
async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    {
        const c = tts.createWsConnector(wsConfig({ vpnPolicy: 'force-physical' }));
        try {
            const conn = c.createConnection();
            const evs = collect(conn);
            let err = null;
            try {
                await withTimeout(conn.connect(wssUrl('/echo'), 5000), 9000, 'I1');
            } catch (e) { err = e; }
            assert.ok(err, 'force-physical 连环回居然成功了');
            console.log(`      result=${err.result} errName=${err.errName}`);
            console.log(`      errMessage=${err.errMessage}`);
            assert.strictEqual(err.result, 70);
            assert.strictEqual(err.errName, 'BC_R_ROUTE_MISMATCH');
            assert.match(err.errMessage, /force-physical/);
            assert.match(err.errMessage, /vpnPolicy=os|prefer-physical/);
            assert.match(err.errMessage, /127\.0\.0\.1/);
            assert.match(err.errMessage, /ifIndex=\d+/);
            assert.match(err.errMessage, /peerClass=0x/);
            // 事后复查同一段文案
            assert.match(conn.info().lastError, /ifIndex=\d+/);
            // 握手失败 -> 只有 reject，没有 closed（契约第 2 条，业务据此分支重连）
            await sleep(400);
            assert.strictEqual(evs.closed.length, 0, '握手失败居然还发了 closed');
            // 这种连接的 close() 必须立刻 resolve（closed 永远不会来）
            const cost = await resolvesWithin(conn.close(), 1000, 'I-close-failed');
            console.log(`      握手失败的连接 close() 立刻 resolve（${cost}ms）`);
        } finally {
            await c.close();
        }
    }
    {
        const c = tts.createWsConnector(wsConfig({ vpnPolicy: 'prefer-physical' }));
        try {
            const conn = c.createConnection();
            const evs = collect(conn);
            const ok = await withTimeout(conn.connect(wssUrl('/echo'), 5000), 9000, 'I2');
            console.log(`      prefer-physical -> peerIp=${ok.peerIp} ` +
                        `boundIfIndex=${ok.boundIfIndex} ` +
                        `pinMethod=${ok.pinMethod || '(已放弃绑定)'}`);
            assert.strictEqual(ok.peerIp, '127.0.0.1');
            conn.sendText('prefer');
            await waitFor(() => evs.text.length >= 1, 5000, 'I2-echo');
            assert.strictEqual(evs.text[0], 'prefer');
        } finally {
            await c.close();
        }
    }
});

test('J. 监听器抛异常：进程不死，同批其余事件照常交付', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    // ⚠️ 这条钉住的是一个会打死整个 node 进程的坑：JS 回调抛出的异常留在 env 里，
    // 同一批 uv_async 的下一个事件就会让 C++ 异常逃出事件泵 -> libc++abi:
    // terminating。原生层用 DrainPendingException 修了，胶水层再兜一道（emit 外面
    // 的 try/catch + process.nextTick 重抛）。服务端刻意用**一次 write** 把 6 帧
    // 一起发出来，保证同批。
    const c = tts.createWsConnector(wsConfig());
    const uncaught = [];
    const onUncaught = (e) => uncaught.push(e && e.message);
    process.on('uncaughtException', onUncaught);
    try {
        const conn = c.createConnection();
        const seen = [];
        let first = true;
        conn.on('text', (s) => {
            seen.push(s);
            if (first) { first = false; throw new Error('boom-listener'); }
        });
        // 同一个事件的第二个监听器：EventEmitter 里前一个抛异常会跳过它，
        // 所以这里只断言"后续事件"，不断言同一次 emit 的后续监听器。
        await withTimeout(conn.connect(wssUrl('/burst'), 5000), 9000, 'J-connect');
        conn.sendText('go');
        await waitFor(() => seen.length >= 6, 8000, 'J-burst');
        await sleep(200);
        console.log(`      交付 ${seen.length}/6（${seen.join(',')}）`);
        console.log(`      uncaughtException=${JSON.stringify(uncaught)}`);
        assert.strictEqual(seen.length, 6, '有 text 事件被丢了');
        for (let i = 0; i < 6; i++) assert.strictEqual(seen[i], 'burst-' + i);
        assert.ok(uncaught.includes('boom-listener'),
                  '监听器抛的异常必须报成 uncaughtException，不能悄悄吞掉');
        // 抛过异常之后整条通路仍然可用
        const conn2 = c.createConnection();
        const evs2 = collect(conn2);
        await withTimeout(conn2.connect(wssUrl('/echo'), 5000), 9000, 'J-after');
        conn2.sendText('still-alive');
        await waitFor(() => evs2.text.length >= 1, 5000, 'J-after-echo');
        assert.strictEqual(evs2.text[0], 'still-alive');
        console.log('      抛异常之后新连接照常工作，进程活着');
    } finally {
        process.removeListener('uncaughtException', onUncaught);
        await c.close();
    }
});

test('K. conn.close() 的 Promise 不会悬着（没 connect 过 / 连接器关闭兜底）', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = tts.createWsConnector(wsConfig());
    try {
        // K1 从没 connect 过的连接：按契约永远不会有 closed 事件 -> 必须立刻 resolve
        const idle = c.createConnection();
        const evs = collect(idle);
        const cost = await resolvesWithin(idle.close(), 500, 'K1');
        console.log(`      从没 connect 过的连接 close() ${cost}ms resolve`);
        await sleep(200);
        assert.strictEqual(evs.closed.length, 0,
            '从没 connect 过的连接不该收到 closed（重连逻辑会走错分支）');

        // K2 已连上的连接：close() 与连接器 close() 一起来，两个 Promise 都要 settle
        const live = c.createConnection();
        const liveEvs = collect(live);
        await withTimeout(live.connect(wssUrl('/echo'), 5000), 9000, 'K2-connect');
        const pConn = live.close();
        const pConnector = c.close();
        await withTimeout(Promise.all([pConn, pConnector]), 9000, 'K2');
        console.log(`      conn.close() 与 connector.close() 同时发：都 settle，` +
                    `closed=${liveEvs.closed.length}`);
        assert.strictEqual(liveEvs.closed.length, 1);
    } finally {
        await c.close();
    }
});

test('L. WS 连接器 close() 重复调：两个 Promise 都 settle；close 后不能再建连接',
async () => {
    const c = tts.createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    let closeEvents = 0;
    c.on('close', () => closeEvents++);
    const settled = [];
    const p1 = c.close(); p1.then(() => settled.push(1));
    const p2 = c.close(); p2.then(() => settled.push(2));
    await withTimeout(Promise.all([p1, p2]), 9000, 'L');
    await sleep(50);
    console.log(`      两个 close Promise 都 settle：[${settled.join(',')}] ` +
                `close 事件 ${closeEvents} 次`);
    assert.deepStrictEqual(settled.sort(), [1, 2]);
    assert.strictEqual(closeEvents, 1);
    assert.strictEqual(c.info().closed, true);
    await withTimeout(c.close(), 5000, 'L2');
    assert.throws(() => c.createConnection(), /already closed/);
});

test('M. WS 连接器关闭时在途 connect 的 Promise 必须 reject', async () => {
    const c = tts.createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    const conn = c.createConnection();
    const evs = collect(conn);
    // 黑洞对端：连上了但永远不回 101
    const pending = conn.connect(`ws://127.0.0.1:${servers.silentPort}/x`, 30000);
    await sleep(200);
    await withTimeout(c.close(), 9000, 'M-close');
    let err = null;
    try { await withTimeout(pending, 9000, 'M'); } catch (e) { err = e; }
    assert.ok(err, '连接器关了，在途 connect 的 Promise 居然还成功了');
    console.log(`      在途 connect 被 reject：result=${err.result} ` +
                `errName=${err.errName}`);
    assert.ok(typeof err.result === 'number' && err.result !== 0);
    await sleep(300);
    assert.strictEqual(evs.closed.length, 0, '握手没成功过不该发 closed');
    // 未连上的连接发送要抛（带 result）
    assert.throws(() => conn.sendText('x'), (e) => typeof e.result === 'number');
    assert.throws(() => conn.sendData(Buffer.from('x')), (e) => typeof e.result === 'number');
});

///////////////////////////////////////////////////////////////////////////////
// runner
///////////////////////////////////////////////////////////////////////////////

async function main() {
    await startServers();
    console.log(`胶水层：${GLUE_PATH}`);
    console.log(`原生产物：${ADDON_PATH}`);
    console.log(`本地服务端：http=${servers.plainPort} silent=${servers.silentPort} ` +
                `wss=${servers.wssPort || '(无证书，跳过)'}\n`);

    for (const c of cases) {
        try {
            await c.fn();
            passed++;
            console.log(`  ✓ ${c.name}`);
        } catch (e) {
            if (e instanceof Skip) {
                skipped++;
                console.log(`  - ${c.name}  [跳过] ${e.message}`);
            } else {
                failed++;
                console.log(`  ✗ ${c.name}\n      ${e && e.stack ?
                    e.stack.split('\n').slice(0, 5).join('\n      ') : e}`);
            }
        }
    }

    console.log(`\n通过 ${passed} / 失败 ${failed} / 跳过 ${skipped}`);
    if (shared.ipOs && shared.ipPhysical) {
        console.log(`出口 IP 对照（经胶水层）：os=${shared.ipOs} ` +
                    `force-physical=${shared.ipPhysical} (不同 = 真的走了物理网卡)`);
    }
    stopServers();
    // ⚠️ 必须显式退出：加载 ttsignal 之后 JsExchanger 的 uv_async handle 是 uv_ref
    // 过的，event loop 不会自己空掉（既有行为，不是胶水层引入的）。
    process.exit(failed > 0 ? 1 : 0);
}

main().catch((e) => {
    console.error('测试脚本自身出错：', e);
    stopServers();
    process.exit(1);
});

/*******************************************************************************
 * End of file
 ******************************************************************************/
