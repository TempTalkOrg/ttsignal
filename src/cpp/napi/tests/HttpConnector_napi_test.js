#!/usr/bin/env node
'use strict';

/*******************************************************************************
 * HttpConnector NAPI 绑定的 Node 端到端测试。
 *
 *   node src/cpp/napi/tests/HttpConnector_napi_test.js
 *   node --expose-gc src/cpp/napi/tests/HttpConnector_napi_test.js  # 多跑一条 GC 用例
 *
 * ⚠️ 刻意**直接 require 原生产物**，不经 src/js/index.js：
 *   1. Promise 形态的胶水层是另一个任务的活，这里要验的是原生绑定本身；
 *   2. 跑的必须是"生产实际形态"的那个 .node —— 前面有一轮的集成测试因为编译
 *      命令少了 -DTT_HAS_PATH_MONITOR，网卡绑定整段被条件编译掉、缺陷被构建
 *      配置完整屏蔽。用 cmake 出来的正式产物就不会有这个问题。
 *
 * 公网用例依赖本机的企业 VPN：默认路由走 utun，物理网卡 en0 直连。
 * force-physical 拿到的公网出口 IP 必须与 os 档不同，这是"真的绑到物理网卡"
 * 的最硬证据（比看日志可靠）。VPN 没连时这条会被跳过而不是判失败。
 *
 * 退出码：0 = 全过（含跳过），1 = 有失败。
 ******************************************************************************/

const assert = require('assert');
const fs = require('fs');
const http = require('http');
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
// 把原始接口包一层 Promise。**只是测试用**的最小包装，真正的胶水层在
// src/js/index.js（另一个任务）。
///////////////////////////////////////////////////////////////////////////////

function createHttpConnector(config) {
    return native.__createHttpConnector__(config || {});
}

function request(connector, options) {
    return new Promise((resolve, reject) => {
        connector.__request__(options, (err, resp) => {
            if (err) reject(err); else resolve(resp);
        });
    });
}

function close(connector) {
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

///////////////////////////////////////////////////////////////////////////////
// 测试用的本地服务端
///////////////////////////////////////////////////////////////////////////////

const servers = {};

// certs/ 是 .gitignore 掉的（自签调试证书不进仓库），所以 TLS 那两条用例只在
// 本机确实有证书时跑，缺了就跳过而不是判失败。生成方式：
//
//   openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
//       -keyout certs/localhost.key -out certs/localhost.crt \
//       -subj /CN=localhost -addext subjectAltName=DNS:localhost
const TLS_CERT = path.join(REPO_ROOT, 'certs', 'localhost.crt');
const TLS_KEY = path.join(REPO_ROOT, 'certs', 'localhost.key');
const hasTlsCert = fs.existsSync(TLS_CERT) && fs.existsSync(TLS_KEY);
const NO_TLS_CERT_HINT =
    '本机没有 certs/localhost.{crt,key}（该目录被 .gitignore），' +
    '生成方式见本文件顶部注释';

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
                if (req.url === '/big') {
                    const body = Buffer.alloc(200 * 1024, 0x61);
                    res.writeHead(200, { 'content-type': 'application/octet-stream' });
                    res.end(body);
                    return;
                }
                if (req.url === '/echo') {
                    const payload = JSON.stringify({
                        method: req.method,
                        headers: req.headers,
                        body: Buffer.concat(chunks).toString('utf8')
                    });
                    res.writeHead(200, {
                        'content-type': 'application/json',
                        'x-multi': 'a'
                    });
                    res.end(payload);
                    return;
                }
                res.writeHead(200, 'OK', { 'content-type': 'text/plain' });
                res.end('hello');
            });
        });
        plain.listen(0, '127.0.0.1', () => {
            servers.plain = plain;
            servers.plainPort = plain.address().port;

            // 只 accept、永不应答 —— 用来验请求超时。
            const silent = net.createServer(() => { /* 故意什么都不做 */ });
            silent.listen(0, '127.0.0.1', () => {
                servers.silent = silent;
                servers.silentPort = silent.address().port;

                if (!hasTlsCert) {
                    resolve();
                    return;
                }
                const tls = https.createServer({
                    cert: fs.readFileSync(TLS_CERT),
                    key: fs.readFileSync(TLS_KEY)
                }, (req, res) => {
                    res.writeHead(200, { 'content-type': 'text/plain' });
                    res.end('secure hello');
                });
                tls.on('clientError', () => { /* 校验失败的握手，忽略 */ });
                tls.listen(0, '127.0.0.1', () => {
                    servers.tls = tls;
                    servers.tlsPort = tls.address().port;
                    resolve();
                });
            });
        });
    });
}

function stopServers() {
    ['plain', 'silent', 'tls'].forEach((k) => {
        if (servers[k]) servers[k].close();
    });
}

///////////////////////////////////////////////////////////////////////////////
// 用例框架
///////////////////////////////////////////////////////////////////////////////

const cases = [];
let failed = 0;
let passed = 0;
let skipped = 0;

function test(name, fn) { cases.push({ name, fn }); }

class Skip extends Error {}
function skip(reason) { throw new Skip(reason); }

const PUBLIC_IP_URL = 'https://ipinfo.io/ip';   // ifconfig.me 从物理网卡连不通，别用
const shared = {};

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

test('A. 导出符号齐全 / path monitor 在位 / SMP-QUIC 侧没被动过', async () => {
    assert.strictEqual(typeof native.__createHttpConnector__, 'function');
    assert.strictEqual(typeof native.HttpConnector, 'function');

    // ⚠️ 这条断言是"构建配置悄悄把网卡绑定关掉"这一整类缺陷的运行期防线。
    // 少了 TT_HAS_PATH_MONITOR，_PickPhysicalIfIndex() 恒返回 0：
    // prefer-physical 静默回落系统路由（= VPN 隧道），force-physical 硬失败 64。
    // Task 0b 就是栽在手搓编译命令少了这个宏上，而 CMake 的 configure 期断言
    // 管不到手搓出来的产物 —— 这一条管得到。
    {
        const probe = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
        const info = probe.__info__();
        console.log(`      hasPathMonitor=${info.hasPathMonitor}`);
        assert.strictEqual(info.hasPathMonitor, true,
            '产物缺少 TT_HAS_PATH_MONITOR：网卡绑定整段被条件编译掉了');
        await close(probe);
    }
    // 既有导出必须一个不少（本任务不得破坏 SMP/QUIC 的 NAPI 功能）
    ['__createConnector__', '__createServer__', 'Connector', 'Connection',
     'Server', 'ServerConnection'].forEach((k) => {
        assert.strictEqual(typeof native[k], 'function', `缺少导出 ${k}`);
    });
    // 真的能建出一个 QUIC Connector 对象（比只看符号强一档）
    const quic = native.__createConnector__({
        alpn: 'ttsignal', log_level: 1, taskThreads: 1
    });
    assert.ok(quic, 'createConnector 返回空');
    assert.strictEqual(typeof quic.__createConnection__, 'function');
    assert.strictEqual(typeof quic.__close__, 'function');
    quic.__close__();
});

test('B. 非法 vpnPolicy 当场抛错，不静默回落', async () => {
    assert.throws(() => createHttpConnector({ vpnPolicy: 'force_physical' }),
                  /Invalid vpnPolicy/);
    assert.throws(() => createHttpConnector({ vpnPolicy: 42 }),
                  /Invalid vpnPolicy/);
    // 合法取值要能建出来，且 __info__ 报的是**生效**策略
    const c = createHttpConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    const info = c.__info__();
    assert.strictEqual(info.vpnPolicy, 'force-physical');
    assert.strictEqual(info.closed, false);
    assert.strictEqual(info.pendingRequests, 0);
    await close(c);
    // 形状必须恒定：关闭之后字段一个不少，只是 closed 变 true。少字段会让读到
    // undefined 的人误以为"策略没生效"。
    const after = c.__info__();
    assert.strictEqual(after.closed, true);
    assert.strictEqual(after.vpnPolicy, 'force-physical');
    assert.strictEqual(typeof after.maxResponseBytes, 'number');
    assert.strictEqual(typeof after.drainTimeoutMs, 'number');
    assert.strictEqual(after.hasPathMonitor, true);
    assert.deepStrictEqual(Object.keys(info).sort(), Object.keys(after).sort());
});

test('C. os 档连公网，记下出口 IP', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const r = await withTimeout(
            request(c, { url: PUBLIC_IP_URL, timeoutMs: 15000 }), 20000, 'C');
        assert.strictEqual(r.status, 200);
        assert.strictEqual(r.reason, 'OK');
        assert.ok(Buffer.isBuffer(r.body), 'body 应该是 Buffer');
        shared.ipOs = r.body.toString('utf8').trim();
        assert.match(shared.ipOs, /^\d+\.\d+\.\d+\.\d+$/);
        console.log(`      os              -> peerIp=${r.peerIp} ` +
                    `boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod || '(未绑定)'} 出口IP=${shared.ipOs}`);
    } finally {
        await close(c);
    }
});

test('D. force-physical 连公网，出口 IP 必须与 os 档不同', async () => {
    const c = createHttpConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    try {
        const r = await withTimeout(
            request(c, { url: PUBLIC_IP_URL, timeoutMs: 15000 }), 20000, 'D');
        assert.strictEqual(r.status, 200);
        const ip = r.body.toString('utf8').trim();
        console.log(`      force-physical  -> peerIp=${r.peerIp} ` +
                    `boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod} 出口IP=${ip}`);
        // 这三项是本任务刻意暴露到 JS 的：业务无需翻日志就能判断走没走物理网卡
        assert.ok(r.boundIfIndex > 0, 'boundIfIndex 为 0 = 根本没绑网卡');
        assert.ok(r.pinMethod.length > 0, 'pinMethod 为空 = 没绑');
        assert.match(ip, /^\d+\.\d+\.\d+\.\d+$/);
        if (!shared.ipOs) skip('用例 C 没拿到 os 档 IP，无法对照');
        if (shared.ipOs === ip) {
            skip(`os 与 force-physical 出口 IP 相同（${ip}）——` +
                 '本机此刻大概没连 VPN，这条对照失去意义');
        }
        shared.ipPhysical = ip;
    } finally {
        await close(c);
    }
});

test('E. force-physical 连环回必须失败，且错误码是 70（可诊断）', async () => {
    const c = createHttpConnector({ vpnPolicy: 'force-physical', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/ok`;
        await assert.rejects(
            withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'E'),
            (err) => {
                console.log(`      result=${err.result} errName=${err.errName}`);
                console.log(`      errMessage=${err.errMessage.slice(0, 120)}...`);
                assert.strictEqual(err.result, 70, 'BC_R_ROUTE_MISMATCH 必须原样透传');
                assert.strictEqual(err.errName, 'BC_R_ROUTE_MISMATCH');
                // "失败可诊断"是 force-physical 的验收项：文案里要说清怎么改
                assert.match(err.errMessage, /force-physical/);
                assert.match(err.errMessage, /vpnPolicy=os|prefer-physical/);
                return true;
            });
    } finally {
        await close(c);
    }
});

test('F. prefer-physical 连环回应当成功（会自动放弃绑定）', async () => {
    const c = createHttpConnector({ vpnPolicy: 'prefer-physical', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/ok`;
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'F');
        assert.strictEqual(r.status, 200);
        assert.strictEqual(r.body.toString('utf8'), 'hello');
        assert.strictEqual(r.peerIp, '127.0.0.1');
        console.log(`      boundIfIndex=${r.boundIfIndex} ` +
                    `pinMethod=${r.pinMethod || '(已放弃绑定)'}`);
    } finally {
        await close(c);
    }
});

test('G. 404 是正常响应，不是错误', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/notfound`;
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'G');
        assert.strictEqual(r.status, 404);
        assert.strictEqual(r.reason, 'Not Found');
        assert.strictEqual(r.body.toString('utf8'), 'nope');
        assert.strictEqual(r.headers['content-type'], 'text/plain');
        // 头对象必须是自有属性，不能被对端用 __proto__ 之类的头名污染原型
        assert.strictEqual(Object.prototype.hasOwnProperty.call(
            r.headers, 'content-type'), true);
    } finally {
        await close(c);
    }
});

test('H. 请求超时（只 accept 不应答的对端）', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.silentPort}/`;
        const started = Date.now();
        await assert.rejects(
            withTimeout(request(c, { url, timeoutMs: 800 }), 6000, 'H'),
            (err) => {
                const cost = Date.now() - started;
                console.log(`      ${cost}ms result=${err.result} ` +
                            `errName=${err.errName} errMessage=${err.errMessage}`);
                assert.ok(typeof err.result === 'number' && err.result !== 0);
                assert.ok(cost < 5000, '超时没在 timeoutMs 附近生效');
                return true;
            });
    } finally {
        await close(c);
    }
});

test('I. TLS 校验失败（自签证书 + 系统信任库）', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `https://localhost:${servers.tlsPort}/`;
        await assert.rejects(
            withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'I'),
            (err) => {
                console.log(`      result=${err.result} errName=${err.errName} ` +
                            `errMessage=${err.errMessage.slice(0, 140)}`);
                assert.strictEqual(err.result, 72, 'BC_R_TLS_VERIFY_FAILED');
                assert.strictEqual(err.errName, 'BC_R_TLS_VERIFY_FAILED');
                return true;
            });
    } finally {
        await close(c);
    }
});

test('J. 显式 caCerts 让同一个自签服务端校验通过', async () => {
    if (!hasTlsCert) skip(NO_TLS_CERT_HINT);
    const pem = fs.readFileSync(TLS_CERT, 'utf8');
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1, caCerts: pem });
    try {
        const url = `https://localhost:${servers.tlsPort}/`;
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'J');
        assert.strictEqual(r.status, 200);
        assert.strictEqual(r.body.toString('utf8'), 'secure hello');
    } finally {
        await close(c);
    }
});

test('K. POST：method / headers / body 完整送达对端', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/echo`;
        const r = await withTimeout(request(c, {
            method: 'POST',
            url,
            headers: { authorization: 'Bearer t0ken', 'x-custom': 'yes' },
            body: Buffer.from('{"a":1}', 'utf8'),
            timeoutMs: 4000
        }), 8000, 'K');
        assert.strictEqual(r.status, 200);
        const echoed = JSON.parse(r.body.toString('utf8'));
        assert.strictEqual(echoed.method, 'POST');
        assert.strictEqual(echoed.headers.authorization, 'Bearer t0ken');
        assert.strictEqual(echoed.headers['x-custom'], 'yes');
        assert.strictEqual(echoed.headers['content-length'], '7');
        assert.strictEqual(echoed.body, '{"a":1}');
    } finally {
        await close(c);
    }
});

test('L. 请求头里的 CRLF 按注入拦下，错误码 = BC_R_INVALIDARG', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/ok`;
        await assert.rejects(
            withTimeout(request(c, {
                url, headers: { 'x-evil': 'a\r\nX-Injected: 1' }, timeoutMs: 4000
            }), 8000, 'L'),
            (err) => {
                console.log(`      result=${err.result} errName=${err.errName} ` +
                            `errMessage=${err.errMessage}`);
                assert.strictEqual(err.errName, 'BC_R_INVALIDARG');
                return true;
            });
        // URL 非法同样走回调而不是抛异常
        await assert.rejects(
            withTimeout(request(c, { url: 'ftp://x/y', timeoutMs: 1000 }), 8000, 'L2'),
            (err) => {
                console.log(`      ftp:// -> ${err.errName}: ${err.errMessage}`);
                assert.strictEqual(err.errName, 'BC_R_INVALIDARG');
                return true;
            });
    } finally {
        await close(c);
    }
});

test('M. maxResponseBytes 超限 -> 73 BC_R_RESPONSE_TOO_LARGE', async () => {
    const c = createHttpConnector({
        vpnPolicy: 'os', logLevel: 1, maxResponseBytes: 4096
    });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/big`;
        assert.strictEqual(c.__info__().maxResponseBytes, 4096);
        await assert.rejects(
            withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'M'),
            (err) => {
                console.log(`      result=${err.result} errName=${err.errName}`);
                assert.strictEqual(err.result, 73);
                assert.strictEqual(err.errName, 'BC_R_RESPONSE_TOO_LARGE');
                return true;
            });
    } finally {
        await close(c);
    }
});

test('N. 一个连接器串行 + 并发多请求，全部 settle', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/ok`;
        for (let i = 0; i < 3; i++) {
            const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'N1');
            assert.strictEqual(r.status, 200);
        }
        const all = await withTimeout(Promise.all([
            request(c, { url, timeoutMs: 4000 }),
            request(c, { url, timeoutMs: 4000 }),
            request(c, { url, timeoutMs: 4000 })
        ]), 10000, 'N2');
        all.forEach((r) => assert.strictEqual(r.status, 200));
        assert.strictEqual(c.__info__().pendingRequests, 0);
    } finally {
        await close(c);
    }
});

test('O. close 时在途请求必须被 reject，不能让 Promise 永远悬着', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    const url = `http://127.0.0.1:${servers.silentPort}/`;
    const pending = request(c, { url, timeoutMs: 30000 });
    // 让请求真的发出去再关
    await new Promise((r) => setTimeout(r, 200));
    const closed = close(c);
    await assert.rejects(withTimeout(pending, 8000, 'O'), (err) => {
        console.log(`      在途请求被 reject：result=${err.result} ` +
                    `errName=${err.errName}`);
        return true;
    });
    await withTimeout(closed, 8000, 'O-close');
    assert.strictEqual(c.__info__().closed, true);
});

test('P. close 之后再发请求 -> 回调报错；close 幂等', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    await close(c);
    await withTimeout(close(c), 5000, 'P-close2');     // 幂等，且仍给信号
    const url = `http://127.0.0.1:${servers.plainPort}/ok`;
    await assert.rejects(
        withTimeout(request(c, { url, timeoutMs: 1000 }), 5000, 'P'),
        (err) => {
            console.log(`      result=${err.result} errName=${err.errName} ` +
                        `errMessage=${err.errMessage}`);
            assert.ok(typeof err.result === 'number' && err.result !== 0);
            return true;
        });
});

test('Q. 参数用错（不是请求失败）当场抛异常', async () => {
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        assert.throws(() => c.__request__(), /Invalid arguments/);
        assert.throws(() => c.__request__({ url: 'http://x/' }), /Invalid arguments/);
        assert.throws(() => c.__request__({}, () => {}), /'url' is required/);
        assert.throws(() => c.__request__({ url: 'http://x/', method: 7 }, () => {}),
                      /'method' must be a string/);
        assert.throws(() => c.__request__({ url: 'http://x/', timeoutMs: -1 }, () => {}),
                      /out of range/);
        assert.throws(() => c.__request__(
            { url: 'http://x/', headers: { a: 1 } }, () => {}), /must be a string/);
        assert.throws(() => c.__request__({ url: 'http://x/', body: 5 }, () => {}),
                      /'body' must be a string or a Buffer/);
        assert.strictEqual(c.__info__().pendingRequests, 0, '抛异常的请求不该留下任务');
    } finally {
        await close(c);
    }
});

test('R. log_callback 能收到库日志，且回调回到 Node 主线程', async () => {
    const logs = [];
    const c = createHttpConnector({
        vpnPolicy: 'os',
        logLevel: 4,
        log_callback: (level, msg) => { logs.push([level, msg]); }
    });
    try {
        const url = `http://127.0.0.1:${servers.plainPort}/ok`;
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'R');
        assert.strictEqual(r.status, 200);
        assert.ok(logs.length > 0, '一条库日志都没收到');
        assert.ok(logs.some((l) => /HttpConnector|TcpChannel/.test(l[1])),
                  '收到的日志里没有 HTTP 栈的痕迹');
        console.log(`      收到 ${logs.length} 条日志，例：` +
                    `${String(logs[0][1]).slice(0, 90)}`);
    } finally {
        await close(c);
    }
});

test('S. drainTimeoutMs=0：在途请求照样 settle（该档会静默丢弃在途请求）', async () => {
    const c = createHttpConnector({
        vpnPolicy: 'os', logLevel: 1, drainTimeoutMs: 0
    });
    assert.strictEqual(c.__info__().drainTimeoutMs, 0);
    const pending = request(c, {
        url: `http://127.0.0.1:${servers.silentPort}/`, timeoutMs: 30000
    });
    await new Promise((r) => setTimeout(r, 200));
    await withTimeout(close(c), 8000, 'S-close');
    // 原生层"放弃等待"之后本来就不再回调任何 handler；绑定层必须自己把这些
    // 请求以错误收尾，否则业务的 Promise 永远悬着。
    await assert.rejects(withTimeout(pending, 8000, 'S'), (err) => {
        console.log(`      result=${err.result} errName=${err.errName}`);
        return true;
    });
});

test('T. GC 四种形态都不崩（空闲 / 在途 / close 后 / 日志事件在途）', async () => {
    const url = `http://127.0.0.1:${servers.plainPort}/ok`;
    const gc = () => { if (typeof global.gc === 'function') { global.gc(); global.gc(); } };
    const canGc = typeof global.gc === 'function';

    // T1 用完就丢（只测到"空闲连接器"的析构 —— 这是最弱的一种）
    for (let i = 0; i < 8; i++) {
        const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'T1');
        assert.strictEqual(r.status, 200);
    }

    // T2 **在途 + GC**：请求还没回来就丢引用并强制 GC。Ref()/Unref() 配平错了
    // 的话这里要么崩，要么 Promise 永远不 settle。
    for (let i = 0; i < 10; i++) {
        const p = (() => {
            const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
            return request(c, { url, timeoutMs: 4000 });   // 出了这个作用域就没人引用 c 了
        })();
        gc();
        const r = await withTimeout(p, 8000, 'T2');
        assert.strictEqual(r.status, 200);
    }

    // T3 close 之后再 GC（析构里连接器已经是 NULL 的那条路）
    for (let i = 0; i < 5; i++) {
        const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
        assert.strictEqual((await withTimeout(request(c, { url, timeoutMs: 4000 }),
                                              8000, 'T3')).status, 200);
        await close(c);
        gc();
    }

    // T4 **日志事件在途时 GC**：log_callback 那条路上对象不持 Ref，是唯一
    // "事件已进队但对象可能被回收"的窗口。析构里的 RemoveEventByHandler 必须
    // 把它们丢干净。
    let logCount = 0;
    for (let i = 0; i < 10; i++) {
        const p = (() => {
            const c = createHttpConnector({
                vpnPolicy: 'os', logLevel: 4,
                log_callback: () => { logCount++; }
            });
            return request(c, { url, timeoutMs: 4000 });
        })();
        gc();
        await withTimeout(p, 8000, 'T4');
        gc();
    }
    assert.ok(logCount > 0, 'T4 一条日志都没收到，等于没测到这条路');

    // 最后再确认整个 addon 还活着
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    try {
        assert.strictEqual((await withTimeout(request(c, { url, timeoutMs: 4000 }),
                                              8000, 'T5')).status, 200);
    } finally {
        await close(c);
    }
    if (!canGc) skip('上面四种形态都跑过了，但没有 --expose-gc，GC 不是强制触发的');
});

test('U. JS 回调里抛异常：进程不死，同批其余回调照常交付', async () => {
    // ⚠️ 这条钉住的是一个会打死整个 node 进程的坑：macros.h 的 TRY_CATCH_CALL
    // 把 pending exception 留在 env 里没人清，同一批 uv_async 里下一个事件走到
    // Napi::String::New 就抛 C++ 异常、逃出事件泵 -> libc++abi: terminating。
    // 并发请求的回调落在同一批是常态，所以修复前这里是 100% SIGABRT。
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    const url = `http://127.0.0.1:${servers.plainPort}/ok`;
    const seen = [];
    const uncaught = [];
    const onUncaught = (e) => uncaught.push(e && e.message);
    process.on('uncaughtException', onUncaught);
    try {
        const N = 6;
        await withTimeout(new Promise((resolve) => {
            for (let i = 0; i < N; i++) {
                c.__request__({ url, timeoutMs: 4000 }, (err, resp) => {
                    seen.push(err ? ('ERR' + err.result) : resp.status);
                    if (seen.length === N) resolve();
                    if (i === 0) throw new Error('boom');
                });
            }
        }), 15000, 'U');
        console.log(`      交付 ${seen.length}/${N}（${seen.join(',')}）` +
                    ` uncaughtException=${JSON.stringify(uncaught)}`);
        assert.strictEqual(seen.length, N, '有回调被丢了');
        seen.forEach((v) => assert.strictEqual(v, 200));
        assert.ok(uncaught.includes('boom'),
                  '抛出的异常必须被报成 uncaughtException，不能悄悄吞掉');
    } finally {
        process.removeListener('uncaughtException', onUncaught);
        await close(c);
    }
});

test('V. 单条请求的回调抛异常：不污染后续请求（原本会永久静默卡住）', async () => {
    // 修复前的形态：异常从**后面某次毫不相干的 __request__ 调用里**抛出来，
    // 此后所有回调静默丢失，而 uv_ref 又让进程不退出。
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    const url = `http://127.0.0.1:${servers.plainPort}/ok`;
    const uncaught = [];
    const onUncaught = (e) => uncaught.push(e && e.message);
    process.on('uncaughtException', onUncaught);
    try {
        await withTimeout(new Promise((resolve) => {
            c.__request__({ url, timeoutMs: 4000 }, () => {
                resolve();
                throw new Error('boom-single');
            });
        }), 8000, 'V1');
        // 后面这次调用本身不能抛，回调也必须照常来
        const r = await withTimeout(request(c, { url, timeoutMs: 4000 }), 8000, 'V2');
        assert.strictEqual(r.status, 200);
        assert.ok(uncaught.includes('boom-single'));
        console.log(`      后续请求 status=${r.status} ` +
                    `uncaughtException=${JSON.stringify(uncaught)}`);
    } finally {
        process.removeListener('uncaughtException', onUncaught);
        await close(c);
    }
});

test('W. __close__ 调两次，两个回调都要触发', async () => {
    // 单个引用被无条件 Reset 覆盖的话，第一个回调永不触发 —— Task 3 把 close
    // 包成 Promise 之后那就是"第一个 Promise 永远悬着"。
    const c = createHttpConnector({ vpnPolicy: 'os', logLevel: 1 });
    let a = false, b = false;
    await withTimeout(new Promise((resolve) => {
        let n = 0;
        const done = () => { if (++n === 2) resolve(); };
        c.__close__(() => { a = true; done(); });
        c.__close__(() => { b = true; done(); });
    }), 8000, 'W');
    console.log(`      a=${a} b=${b}`);
    assert.strictEqual(a, true, '第一个 close 回调被静默丢了');
    assert.strictEqual(b, true, '第二个 close 回调没触发');
    // 关完之后再调一次也要给信号
    await withTimeout(close(c), 5000, 'W2');
});

///////////////////////////////////////////////////////////////////////////////
// runner
///////////////////////////////////////////////////////////////////////////////

async function main() {
    await startServers();
    console.log(`原生产物：${ADDON_PATH}`);
    console.log(`本地服务端：http=${servers.plainPort} ` +
                `silent=${servers.silentPort} https=${servers.tlsPort || "(无证书，跳过)"}\n`);

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
                    e.stack.split('\n').slice(0, 4).join('\n      ') : e}`);
            }
        }
    }

    console.log(`\n通过 ${passed} / 失败 ${failed} / 跳过 ${skipped}`);
    if (shared.ipOs && shared.ipPhysical) {
        console.log(`出口 IP 对照：os=${shared.ipOs} ` +
                    `force-physical=${shared.ipPhysical} (不同 = 真的绑到了物理网卡)`);
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
