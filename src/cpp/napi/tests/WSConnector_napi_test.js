#!/usr/bin/env node
'use strict';

/*******************************************************************************
 * WSConnector NAPI 绑定的 Node 端到端测试。
 *
 *   node src/cpp/napi/tests/WSConnector_napi_test.js
 *   node --expose-gc src/cpp/napi/tests/WSConnector_napi_test.js  # GC 用例才是强制触发的
 *
 * ⚠️ 刻意**直接 require 原生产物**，不经 src/js/index.js：
 *   1. EventEmitter + Promise 的胶水层是另一个任务的活，这里要验的是原生绑定本身；
 *   2. 跑的必须是"生产实际形态"的那个 .node —— 前面有一轮的集成测试因为编译
 *      命令少了 -DTT_HAS_PATH_MONITOR，网卡绑定整段被条件编译掉、缺陷被构建
 *      配置完整屏蔽。用 cmake 出来的正式产物就不会有这个问题（用例 A 还会在
 *      运行期再核一遍 hasPathMonitor）。
 *
 * ⚠️ 本地 wss 服务端是**手写**的（握手 + 帧编解码），不用 ws 模块：
 *   1. 本仓 node_modules 里没有 ws，测试不该为此引入新依赖；
 *   2. 三种握手失败形态（非 101 / 缺 Sec-WebSocket-Accept / accept 值错）与
 *      "发一个 RSV1 置位的违规帧"，ws 模块压根造不出来；
 *   3. 手写服务端才能顺手断言"客户端发出的帧都带掩码"（RFC 6455 5.1，
 *      WSConnector 从 jmp 移植时修的正是这一条）。
 *
 * "os vs force-physical 的 boundIfIndex 对照"必须打公网：force-physical 连环回
 * 一定失败（那正是用例 E 要验的），所以那条对照走 wss://echo.websocket.org/。
 * 该服务偶尔抖动，连不上时按跳过处理而不是判失败。
 *
 * 退出码：0 = 全过（含跳过），1 = 有失败。
 ******************************************************************************/

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const https = require('https');
const net = require('net');
const path = require('path');

///////////////////////////////////////////////////////////////////////////////
// 加载原生产物
///////////////////////////////////////////////////////////////////////////////

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..', '..');
const ADDON_PATH = process.env.TTSIGNAL_NATIVE_PATH || path.join(
    REPO_ROOT, 'node_modules', 'ttsignal', 'dist', 'build', 'Debug',
    `ttsignal.${process.platform}.${process.arch}.node`);

if (!fs.existsSync(ADDON_PATH)) {
    console.error(`找不到原生产物：${ADDON_PATH}`);
    console.error('先跑 build/macos-arm64-debug/build（或对应平台的构建脚本）。');
    process.exit(1);
}
const native = require(ADDON_PATH);

///////////////////////////////////////////////////////////////////////////////
// 最小胶水层。**只是测试用**，真正的 EventEmitter 化在 src/js/index.js（下一个任务）。
//
// ⚠️ _internalCallback 必须在**建任何连接之前**装到原型上：绑定层是在构造函数里
// 从原型上取一次的（同 JsSMPConnectionWrap 的既有做法）。
///////////////////////////////////////////////////////////////////////////////

native.WsConnection.prototype._internalCallback = function (type, arg) {
    const evs = this.__events;
    if (evs) {
        if (!evs[type]) evs[type] = [];
        evs[type].push(arg);
    }
    const hooks = this.__hooks;
    if (hooks && typeof hooks[type] === 'function') {
        hooks[type](arg);            // 抛异常的用例靠这个入口
    }
};

native.WsConnector.prototype._internalCallback = function (type, arg) {
    const evs = this.__events;
    if (evs) {
        if (!evs[type]) evs[type] = [];
        evs[type].push(arg);
    }
};

function createWsConnector(config) {
    return native.__createWsConnector__(config || {});
}

// 建连接并挂上事件收集器。返回 { conn, evs, hooks }
function newConnection(connector, config) {
    const conn = connector.__createConnection__(config);
    const evs = { text: [], data: [], closed: [], exception: [] };
    const hooks = {};
    conn.__events = evs;
    conn.__hooks = hooks;
    return { conn, evs, hooks };
}

function connect(conn, url, timeoutMs) {
    return new Promise((resolve, reject) => {
        conn.__connect__(url, timeoutMs, (err, ok) => {
            if (err) reject(err); else resolve(ok);
        });
    });
}

function closeConnector(connector) {
    return new Promise((resolve) => connector.__close__(resolve));
}

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

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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

///////////////////////////////////////////////////////////////////////////////
// 手写的本地 wss 服务端
///////////////////////////////////////////////////////////////////////////////

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OP_TEXT = 0x1, OP_BINARY = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xa;

function wsAcceptOf(key) {
    return crypto.createHash('sha1').update(String(key) + WS_GUID).digest('base64');
}

function encodeFrame(opcode, payload, rsv1) {
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
    // 服务端发出的帧不掩码（RFC 6455 5.1 只要求客户端掩码）
    header[0] = 0x80 | (rsv1 ? 0x40 : 0) | opcode;
    return Buffer.concat([header, payload]);
}

// 服务端侧解帧。WSConnector 的 SendText / SendData 发的都是单帧不分片，所以这里
// 不做分片重组（真收到分片会当独立帧记下来，测试里会看出来）。
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
                if (buf.readUInt32BE(off) !== 0) return;   // > 4GiB，测试里不会有
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
            onFrame({
                fin: (b0 & 0x80) !== 0,
                rsv: (b0 & 0x70) >> 4,
                opcode: b0 & 0x0f,
                masked,
                payload
            });
        }
    };
}

const servers = {};
// 服务端观测到的一切，用例按 path 过滤。这是"客户端行为"的唯一可信证据来源。
let wsLog = [];
function logged(p, kind) {
    return wsLog.filter((e) => e.path === p && e.kind === kind);
}

const TLS_CERT = path.join(REPO_ROOT, 'certs', 'localhost.crt');
const TLS_KEY = path.join(REPO_ROOT, 'certs', 'localhost.key');
const hasTlsCert = fs.existsSync(TLS_CERT) && fs.existsSync(TLS_KEY);
const NO_TLS_CERT_HINT =
    '本机没有 certs/localhost.{crt,key}（该目录被 .gitignore），生成方式：' +
    'openssl req -x509 -newkey rsa:2048 -nodes -days 365 ' +
    '-keyout certs/localhost.key -out certs/localhost.crt -subj /CN=localhost ' +
    '-addext subjectAltName=DNS:localhost';
const CA_PEM = hasTlsCert ? fs.readFileSync(TLS_CERT, 'utf8') : '';

function handleUpgrade(req, socket) {
    const url = req.url || '/';
    const p = url.split('?')[0];
    const key = req.headers['sec-websocket-key'];
    const send = (opcode, payload, rsv1) =>
        socket.write(encodeFrame(opcode, payload || Buffer.alloc(0), rsv1));

    socket.on('error', () => { /* 客户端随时可能 RST，忽略 */ });
    wsLog.push({ path: p, kind: 'upgrade', info: req.headers });

    // ---- 三种握手失败形态 ------------------------------------------------
    if (p === '/bad-status') {
        socket.end('HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n' +
                   'Connection: close\r\n\r\n');
        return;
    }
    if (p === '/no-accept') {
        // 101 + Upgrade/Connection 都对，就是**没有** Sec-WebSocket-Accept
        socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                     'Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n');
        return;
    }
    if (p === '/bad-accept') {
        socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                     'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                     'Sec-WebSocket-Accept: ' + wsAcceptOf('not-the-real-key') +
                     '\r\n\r\n');
        return;
    }

    // ---- 正常握手 --------------------------------------------------------
    socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
                 'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                 'Sec-WebSocket-Accept: ' + wsAcceptOf(key) + '\r\n' +
                 'X-Test-Server: ttsignal\r\n\r\n');

    socket.on('data', makeDecoder((f) => {
        if (!f.masked && f.opcode !== 0) {
            // RFC 6455 5.1：客户端发出的每一帧都必须掩码。原版 jmp 实现不掩码，
            // 合规服务端会以 1002 关连接 —— 这条记录就是"确实掩码了"的证据。
            wsLog.push({ path: p, kind: 'unmasked', info: f.opcode });
        }
        switch (f.opcode) {
        case OP_TEXT:
            wsLog.push({ path: p, kind: 'text', info: f.payload.toString('utf8') });
            if (p === '/echo') send(OP_TEXT, f.payload);
            if (p === '/burst' && f.payload.toString('utf8') === 'go') {
                // ⚠️ **一次 write 把 6 帧一起送出去**：保证客户端一次 read 就收全，
                // 于是 6 个事件落在同一批 uv_async 里 —— 那正是"回调里抛异常会
                // 打死进程"这个坑的必现形态。
                const frames = [];
                for (let i = 0; i < 6; i++) {
                    frames.push(encodeFrame(OP_TEXT, Buffer.from('burst-' + i)));
                }
                socket.write(Buffer.concat(frames));
            }
            break;
        case OP_BINARY:
            wsLog.push({ path: p, kind: 'binary', info: f.payload });
            if (p === '/echo') send(OP_BINARY, f.payload);
            break;
        case OP_PING:
            wsLog.push({ path: p, kind: 'ping', info: f.payload });
            send(OP_PONG, f.payload);
            break;
        case OP_PONG:
            wsLog.push({ path: p, kind: 'pong', info: f.payload });
            break;
        case OP_CLOSE:
            wsLog.push({ path: p, kind: 'close', info: f.payload });
            if (p !== '/close-now') send(OP_CLOSE, f.payload);
            socket.end();
            break;
        default:
            break;
        }
    }));
    socket.on('close', () => wsLog.push({ path: p, kind: 'socket-close' }));

    // ---- 握手之后由服务端主动做点什么 -------------------------------------
    if (p === '/close-now') {
        // 对端主动发起关闭握手
        const body = Buffer.alloc(2);
        body.writeUInt16BE(1001, 0);
        setTimeout(() => send(OP_CLOSE, body), 80);
    }
    if (p === '/server-ping') {
        setTimeout(() => send(OP_PING, Buffer.from('sping')), 80);
    }
    if (p === '/push') {
        // 客户端什么都不发，服务端主动推一条文本再关 —— 用来验"连接 JS 对象被
        // GC 之后事件还能不能交付"（Ref() 钉住的意义就在这里）。
        setTimeout(() => send(OP_TEXT, Buffer.from('pushed')), 150);
        setTimeout(() => {
            const body = Buffer.alloc(2);
            body.writeUInt16BE(1000, 0);
            send(OP_CLOSE, body);
        }, 400);
    }
    if (p === '/bad-frame') {
        // RSV1 置位 = 协议违规（没协商扩展）。WSParser 会 throw BCException，
        // 绑定层要把它转成 exception 事件，随后走 closed。
        setTimeout(() => send(OP_TEXT, Buffer.from('nope'), true), 80);
    }
}

function startServers() {
    return new Promise((resolve, reject) => {
        if (!hasTlsCert) { resolve(); return; }
        const tls = https.createServer({
            cert: fs.readFileSync(TLS_CERT),
            key: fs.readFileSync(TLS_KEY)
        });
        tls.on('upgrade', handleUpgrade);
        tls.on('request', (req, res) => { res.writeHead(426); res.end(); });
        tls.on('clientError', () => { /* 校验失败的握手，忽略 */ });
        tls.on('error', reject);
        tls.listen(0, '127.0.0.1', () => {
            servers.wss = tls;
            servers.wssPort = tls.address().port;

            // 只 accept、永不应答 —— 用来验握手响应超时那条预算
            const silent = net.createServer(() => { /* 故意什么都不做 */ });
            silent.listen(0, '127.0.0.1', () => {
                servers.silent = silent;
                servers.silentPort = silent.address().port;
                resolve();
            });
        });
    });
}

function stopServers() {
    ['wss', 'silent'].forEach((k) => { if (servers[k]) servers[k].close(); });
}

function wssUrl(p) { return `wss://localhost:${servers.wssPort}${p}`; }

///////////////////////////////////////////////////////////////////////////////
// 用例框架
///////////////////////////////////////////////////////////////////////////////

const cases = [];
let failed = 0, passed = 0, skipped = 0;

function test(name, fn) { cases.push({ name, fn }); }

class Skip extends Error {}
function skip(reason) { throw new Skip(reason); }

const PUBLIC_WSS = 'wss://echo.websocket.org/';
const shared = {};

// 本地 wss 用自签证书，一律显式给 caCerts（不用 insecureSkipVerify —— 那会把
// 证书校验整条路绕过去，等于少测一块）。
function localConfig(extra) {
    return Object.assign({ vpnPolicy: 'os', logLevel: 1, caCerts: CA_PEM }, extra);
}

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

test('A. 导出符号齐全 / path monitor 在位 / 既有绑定没被动过', async () => {
    assert.strictEqual(typeof native.__createWsConnector__, 'function');
    assert.strictEqual(typeof native.WsConnector, 'function');
    assert.strictEqual(typeof native.WsConnection, 'function');

    // ⚠️ 这条断言是"构建配置悄悄把网卡绑定关掉"这一整类缺陷的运行期防线。
    // 少了 TT_HAS_PATH_MONITOR，_PickPhysicalIfIndex() 恒返回 0：
    // prefer-physical 静默回落系统路由（= VPN 隧道），force-physical 硬失败 64。
    // CMake 的 configure 期断言管不到手搓编译命令编出来的产物 —— 这一条管得到。
    {
        const probe = createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
        const info = probe.__info__();
        console.log(`      hasPathMonitor=${info.hasPathMonitor} ` +
                    `vpnPolicy=${info.vpnPolicy} drainTimeoutMs=${info.drainTimeoutMs}`);
        assert.strictEqual(info.hasPathMonitor, true,
            '产物缺少 TT_HAS_PATH_MONITOR：网卡绑定整段被条件编译掉了');
        // 连接级 __info__ 也要有，理由同上
        const { conn, evs } = newConnection(probe);
        assert.strictEqual(conn.__info__().hasPathMonitor, true);
        assert.strictEqual(conn.__info__().upgraded, false);
        await closeConnector(probe);
        await sleep(200);
        // ⚠️ 从没 connect 过的连接在连接器收尾时**不能**收到 closed（契约第 2 条：
        // 握手没成功就没有"连接关闭"这件事）。绑定层的兜底路径最容易在这里多发
        // 一个事件，业务的重连逻辑会因此走错分支。
        assert.strictEqual(evs.closed.length, 0,
            '从没连接过的 connection 居然收到了 closed');
        assert.strictEqual(evs.exception.length, 0);
    }
    // 既有导出必须一个不少（本任务不得破坏 SMP/QUIC 与 HTTP 的 NAPI 功能）
    ['__createConnector__', '__createServer__', '__createHttpConnector__',
     'Connector', 'Connection', 'Server', 'ServerConnection',
     'HttpConnector'].forEach((k) => {
        assert.strictEqual(typeof native[k], 'function', `缺少导出 ${k}`);
    });
    const quic = native.__createConnector__({
        alpn: 'ttsignal', log_level: 1, taskThreads: 1
    });
    assert.ok(quic, 'createConnector 返回空');
    quic.__close__();
    const http = native.__createHttpConnector__({ vpnPolicy: 'os', logLevel: 1 });
    assert.strictEqual(http.__info__().hasPathMonitor, true);
    await new Promise((r) => http.__close__(r));
});

test('B. 非法 vpnPolicy 当场抛错，不静默回落；__info__ 形状恒定', async () => {
    assert.throws(() => createWsConnector({ vpnPolicy: 'force_physical' }),
                  /Invalid vpnPolicy/);
    assert.throws(() => createWsConnector({ vpnPolicy: 42 }),
                  /Invalid vpnPolicy/);
    const c = createWsConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    // 连接级 config 也要校验：这里拼错同样是"静默失去真实 IP 保证"
    assert.throws(() => c.__createConnection__({ vpnPolicy: 'force_physical' }),
                  /Invalid vpnPolicy/);
    const info = c.__info__();
    assert.strictEqual(info.vpnPolicy, 'force-physical');
    assert.strictEqual(info.closed, false);
    await closeConnector(c);
    // 形状必须恒定：关闭之后字段一个不少，只是 closed 变 true。少字段会让读到
    // undefined 的人误以为"策略没生效"。
    const after = c.__info__();
    assert.strictEqual(after.closed, true);
    assert.strictEqual(after.vpnPolicy, 'force-physical');
    assert.strictEqual(after.hasPathMonitor, true);
    assert.deepStrictEqual(Object.keys(info).sort(), Object.keys(after).sort());
    // close 之后不能再建连接
    assert.throws(() => c.__createConnection__(), /already closed/);
});

test('C. 本地 wss 回声：文本 / 二进制 / 客户端掩码 / 诊断字段', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        const { conn, evs } = newConnection(c);
        const ok = await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'C');
        console.log(`      peerIp=${ok.peerIp} boundIfIndex=${ok.boundIfIndex} ` +
                    `pinMethod=${ok.pinMethod || '(未绑定)'} ` +
                    `serverHeader=${ok.headers['X-Test-Server'] ||
                                    ok.headers['x-test-server']}`);
        assert.strictEqual(ok.peerIp, '127.0.0.1');
        // os 档不绑网卡
        assert.strictEqual(ok.boundIfIndex, 0);
        assert.strictEqual(typeof ok.headers, 'object');
        assert.ok(Object.keys(ok.headers).some((k) => /sec-websocket-accept/i.test(k)),
                  '握手响应头没带上来');
        assert.strictEqual(conn.__info__().upgraded, true);

        assert.strictEqual(conn.__sendText__('hello-ws'), 0);
        const bin = Buffer.from([0, 1, 2, 250, 255, 0, 7]);
        assert.strictEqual(conn.__sendData__(bin), 0);

        await waitFor(() => evs.text.length >= 1 && evs.data.length >= 1, 5000, 'C-echo');
        assert.strictEqual(evs.text[0], 'hello-ws');
        assert.ok(Buffer.isBuffer(evs.data[0]), 'data 事件应该给 Buffer');
        assert.deepStrictEqual(evs.data[0], bin, '二进制内容必须逐字节相同');
        // RFC 6455 5.1：客户端每一帧都必须掩码
        assert.strictEqual(logged('/echo', 'unmasked').length, 0,
                           '客户端发出了未掩码的帧（合规服务端会以 1002 关连接）');
        assert.strictEqual(logged('/echo', 'text')[0].info, 'hello-ws');

        conn.__close__();
        await waitFor(() => evs.closed.length === 1, 5000, 'C-closed');
        console.log(`      closed reason=${evs.closed[0]}`);
        assert.strictEqual(conn.__info__().closed, true);
        // 关掉之后诊断字段仍然报得出来（形状恒定）
        assert.strictEqual(conn.__info__().peerIp, '127.0.0.1');
    } finally {
        await closeConnector(c);
    }
});

test('D. 握手失败三态：只发 connectResult，不发 closed', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    // ⚠️ 这是 WSConnector.h 契约第 2 条要求的语义：握手没成功就没有"连接关闭"
    // 这件事，业务才能把"握手失败"与"连上之后被关掉"分开处理。绑定层必须原样
    // 保持 —— 多发一个 closed 会让重连逻辑走错分支。
    // 第三项是"原生现场描述必须原样到 JS"的断言：这段文案来自
    // WSConnection::CheckHandshakeResponse，绑定层经 LastErrorMessage() 取回。
    // 只有码没有文案的话，业务无法区分"路径写错了"和"对端不是 WS 端点"。
    const forms = [
        ['/bad-status', '响应不是 101（403）', /期望 101 Switching Protocols，实际 403/],
        ['/no-accept', '缺 Sec-WebSocket-Accept', /缺少 Sec-WebSocket-Accept/],
        ['/bad-accept', 'Sec-WebSocket-Accept 值不对', /Sec-WebSocket-Accept 不匹配/]
    ];
    const c = createWsConnector(localConfig());
    try {
        for (const [p, desc, detail] of forms) {
            const { conn, evs } = newConnection(c);
            let err = null;
            try {
                await withTimeout(connect(conn, wssUrl(p), 5000), 9000, 'D' + p);
            } catch (e) { err = e; }
            assert.ok(err, `${desc}：握手居然成功了`);
            console.log(`      ${p.padEnd(12)} ${desc}: result=${err.result} ` +
                        `errName=${err.errName}`);
            assert.strictEqual(err.result, 74,
                'BC_R_WS_HANDSHAKE_FAILED(74) 必须原样透传');
            assert.strictEqual(err.errName, 'BC_R_WS_HANDSHAKE_FAILED');
            assert.ok(err.errMessage.length > 0, 'errMessage 不能是空的');
            console.log(`                   errMessage=${err.errMessage}`);
            assert.match(err.errMessage, detail,
                'errMessage 里没有原生给的现场描述（LastErrorMessage 没接上？）');
            // conn.__info__().lastError 事后也要能复查同一段文案
            assert.match(conn.__info__().lastError, detail);
            // 等一会儿，确认**不会**有 closed 补上来
            await sleep(400);
            assert.strictEqual(evs.closed.length, 0,
                `${desc}：握手失败居然还发了 closed`);
            assert.strictEqual(conn.__info__().upgraded, false);
        }
    } finally {
        await closeConnector(c);
    }
});

test('E. force-physical 连环回必须失败，且错误码是 70（可诊断）', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig({ vpnPolicy: 'force-physical' }));
    try {
        const { conn, evs } = newConnection(c);
        let err = null;
        try {
            await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'E');
        } catch (e) { err = e; }
        assert.ok(err, 'force-physical 连环回居然成功了');
        console.log(`      result=${err.result} errName=${err.errName}`);
        console.log(`      errMessage=${err.errMessage}`);
        assert.strictEqual(err.result, 70, 'BC_R_ROUTE_MISMATCH 必须原样透传');
        assert.strictEqual(err.errName, 'BC_R_ROUTE_MISMATCH');
        // "失败可诊断"是 force-physical 的验收项：文案里要说清怎么改
        assert.match(err.errMessage, /force-physical/);
        assert.match(err.errMessage, /vpnPolicy=os|prefer-physical/);
        // ⚠️ 这三条钉住的是 WSConnection::LastErrorMessage() 真的接上了：**现场
        // 细节**（哪个对端、peerClass 是什么、内核把包判给了哪块网卡）只存在于
        // TcpChannel 生成的那段原生文案里，绑定层按码硬编的通用提示给不出来。
        assert.match(err.errMessage, /127\.0\.0\.1/, '文案里没有对端 IP');
        assert.match(err.errMessage, /ifIndex=\d+/, '文案里没有网卡编号');
        assert.match(err.errMessage, /peerClass=0x\d+/, '文案里没有 peerClass');
        assert.match(conn.__info__().lastError, /ifIndex=\d+/,
                     '__info__().lastError 事后复查不到同一段文案');
        await sleep(300);
        assert.strictEqual(evs.closed.length, 0, '握手前失败不该发 closed');
    } finally {
        await closeConnector(c);
    }
});

test('F. prefer-physical 连环回应当成功（会自动放弃绑定）', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig({ vpnPolicy: 'prefer-physical' }));
    try {
        const { conn, evs } = newConnection(c);
        const ok = await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'F');
        console.log(`      boundIfIndex=${ok.boundIfIndex} ` +
                    `pinMethod=${ok.pinMethod || '(已放弃绑定)'} peerIp=${ok.peerIp}`);
        assert.strictEqual(ok.peerIp, '127.0.0.1');
        assert.strictEqual(conn.__sendText__('prefer'), 0);
        await waitFor(() => evs.text.length >= 1, 5000, 'F-echo');
        assert.strictEqual(evs.text[0], 'prefer');
    } finally {
        await closeConnector(c);
    }
});

test('G. ping/pong 双向', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        // G1 客户端 -> 服务端 ping，服务端回 pong（客户端静默吃掉，只留一条 DEBUG 日志）
        {
            const { conn } = newConnection(c);
            await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'G1');
            assert.strictEqual(conn.__sendPing__(), 0);
            await waitFor(() => logged('/echo', 'ping').length >= 1, 5000, 'G1-ping');
            console.log(`      服务端收到 ping ${logged('/echo', 'ping').length} 个`);
            // pong 回来之后连接照常可用
            assert.strictEqual(conn.__sendText__('after-ping'), 0);
            await waitFor(() => logged('/echo', 'text')
                .some((e) => e.info === 'after-ping'), 5000, 'G1-after');
            conn.__close__();
        }
        // G2 服务端 -> 客户端 ping，客户端必须自动回 pong（RFC 6455 5.5.3）
        {
            const { conn } = newConnection(c);
            await withTimeout(connect(conn, wssUrl('/server-ping'), 5000), 9000, 'G2');
            await waitFor(() => logged('/server-ping', 'pong').length >= 1,
                          5000, 'G2-pong');
            const pong = logged('/server-ping', 'pong')[0].info;
            console.log(`      客户端自动回的 pong payload=${pong.toString('utf8')}`);
            assert.strictEqual(pong.toString('utf8'), 'sping',
                'pong 必须原样回显 ping 的 payload');
            conn.__close__();
        }
    } finally {
        await closeConnector(c);
    }
});

test('H. 对端主动 close：closed 事件到达，且之后不再有事件', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        const { conn, evs } = newConnection(c);
        await withTimeout(connect(conn, wssUrl('/close-now'), 5000), 9000, 'H');
        await waitFor(() => evs.closed.length >= 1, 5000, 'H-closed');
        console.log(`      closed reason=${evs.closed[0]}`);
        assert.strictEqual(evs.closed.length, 1, 'closed 只能发一次');
        assert.match(evs.closed[0], /1001|对端/);
        // 客户端必须把 close 帧回给对端（关闭握手），服务端那边看得到
        await waitFor(() => logged('/close-now', 'close').length >= 1,
                      3000, 'H-echo-close');
        await sleep(300);
        assert.strictEqual(evs.closed.length, 1, 'closed 被重复发了');
        // 关掉之后再发东西只应拿到错误码，不抛
        assert.notStrictEqual(conn.__sendText__('after-close'), 0);
    } finally {
        await closeConnector(c);
    }
});

test('I. 帧解析违规 -> exception 事件，随后 closed', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        const { conn, evs } = newConnection(c);
        await withTimeout(connect(conn, wssUrl('/bad-frame'), 5000), 9000, 'I');
        await waitFor(() => evs.exception.length >= 1, 5000, 'I-exception');
        console.log(`      exception=${String(evs.exception[0]).slice(0, 90)}`);
        assert.strictEqual(typeof evs.exception[0], 'string');
        // 协议违规之后一定收尾
        await waitFor(() => evs.closed.length >= 1, 5000, 'I-closed');
        console.log(`      closed reason=${evs.closed[0]}`);
    } finally {
        await closeConnector(c);
    }
});

test('J. 握手响应超时（只 accept 不应答的对端）', async () => {
    const c = createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const { conn, evs } = newConnection(c);
        let err = null;
        try {
            await withTimeout(
                connect(conn, `ws://127.0.0.1:${servers.silentPort}/x`, 1200),
                9000, 'J');
        } catch (e) { err = e; }
        assert.ok(err, '黑洞服务端居然连上了');
        console.log(`      result=${err.result} errName=${err.errName}`);
        assert.strictEqual(err.errName, 'BC_R_CONNECT_TIMEOUT');
        await sleep(300);
        assert.strictEqual(evs.closed.length, 0, '握手超时不该发 closed');
    } finally {
        await closeConnector(c);
    }
});

test('K. os 档 vs force-physical 档连同一个公网 wss：boundIfIndex 必须不同',
async () => {
    // ⚠️ 这条只能打公网：force-physical 连环回一定失败（用例 E），所以"同一个
    // 服务端两档对照"必须在一个全局可路由的对端上做。boundIfIndex 0 vs 非 0 是
    // "真的绑到了物理网卡"的硬证据（比看日志可靠）。
    const results = {};
    for (const policy of ['os', 'force-physical']) {
        // ⚠️ echo.websocket.org（Fly.io）**会抖**：实测 4 次里有 1~2 次在 TLS 阶段
        // 或 READY 之后直接 FIN（result=24 BC_R_UNEXPECTEDEND），两档都会中。
        // 那是远端问题，不是绑定层的 —— 所以这里重试三次再判，别让一次远端抖动
        // 把"0 vs 14"这条必测项变成跳过。
        for (let attempt = 1; attempt <= 3 && !results[policy]; attempt++) {
            const c = createWsConnector({ vpnPolicy: policy, logLevel: 1 });
            try {
                const { conn, evs } = newConnection(c);
                const ok = await withTimeout(connect(conn, PUBLIC_WSS, 12000),
                                             18000, 'K-' + policy);
                results[policy] = ok;
                console.log(`      ${policy.padEnd(15)}-> peerIp=${ok.peerIp} ` +
                            `boundIfIndex=${ok.boundIfIndex} ` +
                            `pinMethod=${ok.pinMethod || '(未绑定)'}` +
                            (attempt > 1 ? `（第 ${attempt} 次尝试）` : ''));
                // echo.websocket.org 连上会先推一条问候文本，顺手验一下收包路径
                conn.__sendText__('hi-' + policy);
                await waitFor(() => evs.text.length >= 1, 8000, 'K-text-' + policy);
                conn.__close__();
            } catch (e) {
                if (e && e.result !== undefined) {
                    console.log(`      ${policy} 第 ${attempt} 次失败: ` +
                                `result=${e.result} errName=${e.errName}`);
                }
                results[policy] = null;
                results[policy + '_err'] = e;
            } finally {
                await closeConnector(c);
            }
        }
    }
    if (!results.os || !results['force-physical']) {
        skip('公网 wss（echo.websocket.org）本次连不上，两档对照失去意义；' +
             '错误码本身已打印在上面');
    }
    assert.strictEqual(results.os.boundIfIndex, 0,
        'os 档不该绑网卡');
    assert.ok(results['force-physical'].boundIfIndex > 0,
        'force-physical 的 boundIfIndex 为 0 = 根本没绑网卡');
    assert.ok(results['force-physical'].pinMethod.length > 0,
        'pinMethod 为空 = 没绑');
    assert.notStrictEqual(results.os.boundIfIndex,
                          results['force-physical'].boundIfIndex);
    shared.ifOs = results.os.boundIfIndex;
    shared.ifPhysical = results['force-physical'].boundIfIndex;
    shared.pinMethod = results['force-physical'].pinMethod;
    shared.peerOs = results.os.peerIp;
    shared.peerPhysical = results['force-physical'].peerIp;
});

test('L. JS 回调里抛异常：进程不死，同批其余回调照常交付', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    // ⚠️ 这条钉住的是一个会打死整个 node 进程的坑：macros.h 的 TRY_CATCH_CALL 把
    // pending exception 留在 env 里没人清，同一批 uv_async 里下一个事件走到
    // Napi::String::New 就抛 C++ 异常、逃出事件泵 -> libc++abi: terminating。
    // WS 侧比 HTTP 更容易撞上：一次 read 里的多个帧就是多个事件、同一批交付。
    // 服务端刻意用**一次 write** 把 6 帧一起发出来，保证同批。
    const c = createWsConnector(localConfig());
    const uncaught = [];
    const onUncaught = (e) => uncaught.push(e && e.message);
    process.on('uncaughtException', onUncaught);
    try {
        const { conn, evs, hooks } = newConnection(c);
        await withTimeout(connect(conn, wssUrl('/burst'), 5000), 9000, 'L-connect');
        let first = true;
        hooks.text = () => {
            if (first) { first = false; throw new Error('boom'); }
        };
        conn.__sendText__('go');
        await waitFor(() => evs.text.length >= 6, 8000, 'L-burst');
        console.log(`      交付 ${evs.text.length}/6（${evs.text.join(',')}）`);
        console.log(`      uncaughtException=${JSON.stringify(uncaught)}`);
        assert.strictEqual(evs.text.length, 6, '有 text 事件被丢了');
        for (let i = 0; i < 6; i++) {
            assert.strictEqual(evs.text[i], 'burst-' + i);
        }
        assert.ok(uncaught.includes('boom'),
                  '抛出的异常必须被报成 uncaughtException，不能悄悄吞掉');
        // 抛过异常之后整条通路仍然可用
        hooks.text = null;
        const { conn: conn2, evs: evs2 } = newConnection(c);
        await withTimeout(connect(conn2, wssUrl('/echo'), 5000), 9000, 'L-after');
        conn2.__sendText__('still-alive');
        await waitFor(() => evs2.text.length >= 1, 5000, 'L-after-echo');
        assert.strictEqual(evs2.text[0], 'still-alive');
        console.log('      抛异常之后新连接照常工作');
    } finally {
        process.removeListener('uncaughtException', onUncaught);
        await closeConnector(c);
    }
});

test('M. connect 回调里抛异常：不污染后续事件', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    const uncaught = [];
    const onUncaught = (e) => uncaught.push(e && e.message);
    process.on('uncaughtException', onUncaught);
    try {
        const { conn, evs } = newConnection(c);
        await withTimeout(new Promise((resolve, reject) => {
            conn.__connect__(wssUrl('/echo'), 5000, (err) => {
                if (err) { reject(err); return; }
                resolve();
                throw new Error('boom-connect');
            });
        }), 9000, 'M');
        conn.__sendText__('after-throw');
        await waitFor(() => evs.text.length >= 1, 5000, 'M-echo');
        assert.strictEqual(evs.text[0], 'after-throw');
        assert.ok(uncaught.includes('boom-connect'));
        console.log(`      后续 text=${evs.text[0]} ` +
                    `uncaughtException=${JSON.stringify(uncaught)}`);
    } finally {
        process.removeListener('uncaughtException', onUncaught);
        await closeConnector(c);
    }
});

test('N. GC 形态：在途连接 / 连上后丢引用 / 空闲连接器', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const gc = () => { if (typeof global.gc === 'function') { global.gc(); global.gc(); } };
    const canGc = typeof global.gc === 'function';

    // N1 **在途 + 丢引用 + GC**：连接对象出了作用域就没人引用了。Ref() 配平错的话
    // 这里要么崩，要么 connect 的回调永远不来（Promise 悬着）。
    for (let i = 0; i < 6; i++) {
        const c = createWsConnector(localConfig());
        const p = (() => {
            const { conn } = newConnection(c);
            return connect(conn, wssUrl('/echo'), 5000);
        })();
        gc();
        const ok = await withTimeout(p, 9000, 'N1');
        assert.strictEqual(ok.peerIp, '127.0.0.1');
        gc();
        await closeConnector(c);
    }

    // N2 **连上之后丢引用 + GC**：这是 WS 与 HTTP 最不一样的地方 —— 长连接的
    // text / closed 事件必须照常交付，哪怕业务把 conn 变量丢了。钉住到 closed
    // 为止就是为了这个（EventEmitter 形态的连接因为"变量出了作用域"而悄悄不再
    // emit，是个极难查的坑）。
    for (let i = 0; i < 4; i++) {
        const c = createWsConnector(localConfig());
        const evs = { text: [], data: [], closed: [], exception: [] };
        await withTimeout((() => {
            const conn = c.__createConnection__();
            conn.__events = evs;            // 事件收集器留在外面，conn 本身丢掉
            return connect(conn, wssUrl('/push'), 5000);
        })(), 9000, 'N2-connect');
        gc();
        await waitFor(() => evs.text.length >= 1, 6000, 'N2-text');
        assert.strictEqual(evs.text[0], 'pushed');
        gc();
        await waitFor(() => evs.closed.length >= 1, 6000, 'N2-closed');
        gc();
        await closeConnector(c);
    }
    console.log('      N1 在途丢引用 x6 / N2 连上后丢引用 x4：事件全部照常交付');

    // N3 连接器只建不用，直接丢 + GC（析构里 Close + delete 那条路）
    for (let i = 0; i < 5; i++) {
        createWsConnector(localConfig());
        gc();
    }
    // N4 建了连接但从不 connect，然后全丢 + GC（sink 与 conns 的回收路径）
    for (let i = 0; i < 5; i++) {
        const c = createWsConnector(localConfig());
        newConnection(c);
        newConnection(c);
        gc();
    }
    gc();
    await sleep(200);

    // 最后确认整个 addon 还活着
    const c = createWsConnector(localConfig());
    try {
        const { conn, evs } = newConnection(c);
        await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'N5');
        conn.__sendText__('alive');
        await waitFor(() => evs.text.length >= 1, 5000, 'N5-echo');
    } finally {
        await closeConnector(c);
    }
    if (!canGc) skip('上面几种形态都跑过了，但没有 --expose-gc，GC 不是强制触发的');
});

test('O. __close__ 调两次，两个回调都要触发', async () => {
    // 单个引用被无条件 Reset 覆盖的话，第一个回调永不触发 —— 胶水层把 close 包成
    // Promise 之后那就是"第一个 Promise 永远悬着"。
    const c = createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    let a = false, b = false;
    await withTimeout(new Promise((resolve) => {
        let n = 0;
        const done = () => { if (++n === 2) resolve(); };
        c.__close__(() => { a = true; done(); });
        c.__close__(() => { b = true; done(); });
    }), 9000, 'O');
    console.log(`      a=${a} b=${b}`);
    assert.strictEqual(a, true, '第一个 close 回调被静默丢了');
    assert.strictEqual(b, true, '第二个 close 回调没触发');
    // 关完之后再调一次也要给信号
    await withTimeout(closeConnector(c), 5000, 'O2');
});

test('P. 连接器 close 时在途连接必须 settle，不能让 Promise 永远悬着', async () => {
    const c = createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    const { conn, evs } = newConnection(c);
    // 黑洞对端：连上了但永远不回 101
    const pending = connect(conn, `ws://127.0.0.1:${servers.silentPort}/x`, 30000);
    await sleep(200);
    await withTimeout(closeConnector(c), 9000, 'P-close');
    let err = null;
    try { await withTimeout(pending, 9000, 'P'); } catch (e) { err = e; }
    assert.ok(err, '连接器关了，在途连接的 Promise 居然还成功了');
    console.log(`      result=${err.result} errName=${err.errName}`);
    // 握手没成功过 -> 只有 connectResult，没有 closed
    await sleep(300);
    assert.strictEqual(evs.closed.length, 0);
});

test('Q. 第二次 __connect__ 当场抛，不静默换掉回调', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        const { conn } = newConnection(c);
        const p = connect(conn, wssUrl('/echo'), 5000);
        // 静默换掉的话，第一个回调永不触发 —— 与 __close__ 的回调必须成列表是
        // 同一类问题，这里选择当场抛，让调用方立刻看见。
        assert.throws(() => conn.__connect__(wssUrl('/echo'), 5000, () => {}),
                      /already been called/);
        const ok = await withTimeout(p, 9000, 'Q');
        assert.strictEqual(ok.peerIp, '127.0.0.1');
        // 连上之后再 connect 同样抛
        assert.throws(() => conn.__connect__(wssUrl('/echo'), 5000, () => {}),
                      /already been called/);
    } finally {
        await closeConnector(c);
    }
});

test('R. 用法错误当场抛；发送在未连接时只返回错误码', async () => {
    const c = createWsConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const { conn } = newConnection(c);
        assert.throws(() => conn.__connect__(), /Invalid arguments/);
        assert.throws(() => conn.__connect__('ws://x/', 'soon', () => {}),
                      /Invalid arguments/);
        assert.throws(() => conn.__connect__('ws://x/', -1, () => {}),
                      /out of range/);
        assert.throws(() => conn.__sendText__(123), /Invalid arguments/);
        assert.throws(() => conn.__sendData__('not a buffer'), /Invalid arguments/);
        // 还没连上时发东西：返回错误码而不是抛 —— 发送失败是运行期常态，
        // 逼业务给每次 send 套 try/catch 是不合理的。
        assert.notStrictEqual(conn.__sendText__('too early'), 0);
        assert.notStrictEqual(conn.__sendData__(Buffer.from('x')), 0);
        assert.notStrictEqual(conn.__sendPing__(), 0);
        // URL 非法：**异步**经回调报错（不在 __connect__ 的栈里同步触发）
        const { conn: conn2 } = newConnection(c);
        let err = null;
        try {
            await withTimeout(connect(conn2, 'http://localhost/nope', 3000),
                              8000, 'R');
        } catch (e) { err = e; }
        assert.ok(err);
        console.log(`      非法 URL: result=${err.result} errName=${err.errName}`);
        assert.strictEqual(err.errName, 'BC_R_INVALIDARG');
    } finally {
        await closeConnector(c);
    }
});

test('S. log_callback 收得到库日志，且不与 QUIC 侧串台', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const wsLogs = [];
    const quicLogs = [];
    // ⚠️ WSConnector.cpp 注册 appender 时 bExclusive = true，两个方向都不串台。
    // 这条用例把它钉住：QUIC 连接器的 log_callback 不该收到 WS 栈的日志。
    const quic = native.__createConnector__({
        alpn: 'ttsignal', log_level: 4, taskThreads: 1,
        log_callback: (level, msg) => quicLogs.push(String(msg))
    });
    const c = createWsConnector(localConfig({
        logLevel: 4,
        log_callback: (level, msg) => wsLogs.push([level, String(msg)])
    }));
    try {
        const { conn } = newConnection(c);
        await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'S');
        conn.__close__();
        await sleep(300);
        assert.ok(wsLogs.length > 0, '一条库日志都没收到');
        assert.ok(wsLogs.some((l) => /WSConnector|WSConnection|TcpChannel/.test(l[1])),
                  '收到的日志里没有 WS 栈的痕迹');
        console.log(`      WS 侧收到 ${wsLogs.length} 条，例：` +
                    `${wsLogs.find((l) => /WSConnection/.test(l[1]))[1].slice(0, 90)}`);
        const leaked = quicLogs.filter((m) => /WSConnection|WSConnector/.test(m));
        console.log(`      QUIC 侧收到 ${quicLogs.length} 条，其中 WS 相关 ` +
                    `${leaked.length} 条（应为 0）`);
        assert.strictEqual(leaked.length, 0, 'WS 日志串到 QUIC 的 log_callback 了');
    } finally {
        await closeConnector(c);
        quic.__close__();
    }
});

test('T. __stats__ 反映握手失败与在途连接数，close 之后仍报得出来', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    try {
        const { conn } = newConnection(c);
        try { await withTimeout(connect(conn, wssUrl('/bad-accept'), 5000),
                                9000, 'T'); } catch (e) { /* 预期失败 */ }
        const stats = c.__stats__();
        console.log(`      handshake_failed_size=${stats.handshake_failed_size} ` +
                    `allocated=${stats.allocated_conn_size} ` +
                    `active=${stats.active_conn_size}`);
        assert.ok(stats.handshake_failed_size >= 1);
        assert.ok(stats.allocated_conn_size >= 1);
    } finally {
        await closeConnector(c);
    }
    const after = c.__stats__();
    assert.ok(after.handshake_failed_size >= 1,
              'close 之后 __stats__ 应当报最后一次快照，而不是空对象');
    assert.strictEqual(after.active_conn_size, 0);
});

test('U. 连接器 close：已连上的发 closed，从没 connect 过的不发', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createWsConnector(localConfig());
    const live = newConnection(c);
    const idle = newConnection(c);
    await withTimeout(connect(live.conn, wssUrl('/echo'), 5000), 9000, 'U');
    await withTimeout(closeConnector(c), 9000, 'U-close');
    await sleep(300);
    console.log(`      已连上的 closed=${live.evs.closed.length}` +
                `（${live.evs.closed[0]}） 从没连过的 closed=${idle.evs.closed.length}`);
    assert.strictEqual(live.evs.closed.length, 1,
        '连接器 close 之后，已连上的连接必须收到恰好一次 closed');
    assert.strictEqual(idle.evs.closed.length, 0,
        '从没 connect 过的连接不该收到 closed');
});

test('V. 已关闭的连接器再 __close__：同 tick 丢掉最后一个引用 + GC，回调仍须触发',
async () => {
    // ⚠️ 钉住的是"第二次 close 那支忘了 Ref()"：它只补投一个 JWM_CLOSE 事件，若
    // 调用方在同一 tick 丢掉最后一个 JS 引用，析构会 Reset 掉 close 回调列表并
    // RemoveEventByHandler 把刚投的事件丢掉 —— 回调永不触发。胶水层已经把
    // close() 包成 Promise 了，那就是一个永远悬着的 Promise。
    const gc = () => { if (typeof global.gc === 'function') { global.gc(); global.gc(); } };
    const box = { c: createWsConnector({ vpnPolicy: 'os', logLevel: 1 }) };
    await withTimeout(closeConnector(box.c), 9000, 'V1');   // 第一次：真正关掉
    let fired = false;
    // executor 与回调都**只捕获 resolve / fired**，不捕获连接器对象本身
    const p = new Promise((resolve) => {
        box.c.__close__(() => { fired = true; resolve(); });
    });
    box.c = null;           // 同一 tick 丢掉最后一个 JS 引用
    gc();
    await withTimeout(p, 9000, 'V2');
    console.log(`      第二次 close 的回调 fired=${fired}` +
                (typeof global.gc === 'function' ? '（强制 GC 之后）' : '（未开 --expose-gc）'));
    assert.strictEqual(fired, true, '已关闭的连接器再 close，回调被析构路径吞掉了');
});

test('W. 连接器销毁之后：WSConnPtr 已放手，__info__ 仍报连上时的诊断快照',
async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    // 两件事一起钉：
    //  1) 连接器一销毁，连接 wrap 就该放手 WSConnPtr —— 那之后原生 WSConnection
    //     的 handler_ 已成野指针，留着强引用只是让它多活一会儿；
    //  2) 放手之后 __info__() 必须报"连上那一刻"的快照，而不是空值 —— 头文件就是
    //     这么承诺的，而在 wrap 存活期内 m_pConn 从不 reset 的话那段回落是死代码。
    const c = createWsConnector(localConfig({ vpnPolicy: 'prefer-physical' }));
    const { conn } = newConnection(c);
    const ok = await withTimeout(connect(conn, wssUrl('/echo'), 5000), 9000, 'W');
    const before = conn.__info__();
    assert.strictEqual(before.attached, true);
    await withTimeout(closeConnector(c), 9000, 'W-close');
    await sleep(200);
    const after = conn.__info__();
    console.log(`      close 前 attached=${before.attached} peerIp=${before.peerIp} ` +
                `if=${before.boundIfIndex} pin="${before.pinMethod}"`);
    console.log(`      close 后 attached=${after.attached} peerIp=${after.peerIp} ` +
                `if=${after.boundIfIndex} pin="${after.pinMethod}" closed=${after.closed}`);
    // 形状恒定
    assert.deepStrictEqual(Object.keys(before).sort(), Object.keys(after).sort());
    assert.strictEqual(after.attached, false,
        '连接器销毁之后仍握着 WSConnPtr（stale WSConnection 持野 handler_）');
    // 快照必须与连上时 __connect__ 回调给的那份一致
    assert.strictEqual(after.peerIp, ok.peerIp);
    assert.strictEqual(after.boundIfIndex, ok.boundIfIndex);
    assert.strictEqual(after.pinMethod, ok.pinMethod);
    assert.strictEqual(after.upgraded, true);
    assert.strictEqual(after.closed, true);
    // 放手之后发送只拿错误码，不抛
    assert.notStrictEqual(conn.__sendText__('after-detach'), 0);
});

///////////////////////////////////////////////////////////////////////////////
// runner
///////////////////////////////////////////////////////////////////////////////

async function main() {
    await startServers();
    console.log(`原生产物：${ADDON_PATH}`);
    console.log(`本地服务端：wss=${servers.wssPort || '(无证书，跳过)'} ` +
                `silent=${servers.silentPort}`);
    console.log(`GC：${typeof global.gc === 'function' ? '--expose-gc 已开' :
                      '未开（GC 用例会跳过收尾断言）'}\n`);

    for (const c of cases) {
        wsLog = [];                 // 每条用例看自己的服务端观测记录
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
    if (shared.ifPhysical !== undefined) {
        console.log(`boundIfIndex 对照（同一个 wss 服务端）：os=${shared.ifOs} ` +
                    `force-physical=${shared.ifPhysical} ` +
                    `pinMethod=${shared.pinMethod}`);
        console.log(`peerIp：os=${shared.peerOs} ` +
                    `force-physical=${shared.peerPhysical}`);
    }
    stopServers();
    // ⚠️ 必须显式退出：加载 ttsignal 之后 JsExchanger 的 uv_async handle 是
    // uv_ref 过的，event loop 不会自己空掉（既有行为，不是本任务引入的）。
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
