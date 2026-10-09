"use strict";

/*******************************************************************************
 * javascript glue wrapper of ttsignal module
 * @module index.js
 * @author anto.
 ******************************************************************************/

const EventEmitter = require("events").EventEmitter;
const fs = require("fs");
const path = require("path");
const Module = require("module");

const PLATFORM = process.platform;
const ARCH = process.arch;
const ADDON_BASENAME = `ttsignal.${PLATFORM}.${ARCH}.node`;

function expandAsar(candidate) {
  if (!candidate) return candidate;
  const idx = candidate.indexOf(".asar/");
  if (idx === -1) return candidate;
  return candidate.replace(".asar/", ".asar.unpacked/");
}

function resolveAddonPath() {
  const candidates = [];
  if (process.env.TTSIGNAL_NATIVE_PATH) {
    candidates.push(process.env.TTSIGNAL_NATIVE_PATH);
  }

  const baseDir = __dirname;
  const kind = process.ttsBuildType === "debug" ? "Debug" : "Release";
  const searchRoots = [
    path.join(baseDir, "build", kind),
    path.join(baseDir, "dist", "build", kind),
    path.join(baseDir, "..", "build", kind),
    path.join(baseDir, "..", "dist", "build", kind),
  ];

  for (const root of searchRoots) {
    candidates.push(path.join(root, ADDON_BASENAME));
  }

  const unique = [...new Set(candidates.filter(Boolean))];

  for (const candidate of unique) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
    const unpacked = expandAsar(candidate);
    if (unpacked !== candidate && fs.existsSync(unpacked)) {
      return unpacked;
    }
  }

  const tried = unique.concat(unique.map(expandAsar)).filter(Boolean);
  throw new Error(
    `Unable to locate ttsignal native addon (${PLATFORM}/${ARCH}). Tried: ${tried.join(
      ", "
    )}`
  );
}

function loadNativeAddon() {
  const createRequire =
    Module.createRequire ||
    Module.createRequireFromPath ||
    require("module").createRequire;
  const req = createRequire(__filename);
  const addonPath = resolveAddonPath();
  return req(addonPath);
}

const ttsignal = loadNativeAddon();

const CONST = {
    TTS_TYPE_COMMAND			: 0x01,
    TTS_TYPE_MESSAGE			: 0x02,
    TTS_TYPE_USER_CONTROL		: 0x03,
    TTS_CC_BBR                  : 0x62,
    TTS_CC_BBR2                 : 0x42,
    TTS_CC_CUBIC                : 0x63,
    TTS_CC_RENO                 : 0x72,
    LOG_LEVEL_DEBUG             : 0x01,
    LOG_LEVEL_INFO              : 0x02,
    LOG_LEVEL_WARN              : 0x03,
    LOG_LEVEL_ERROR             : 0x04,
    LOG_LEVEL_FATAL             : 0x05
};

//*******************************************************************************
// Inherits
//*******************************************************************************

function copyProperties(target, source) {
    for (var k in source){
        if (source.hasOwnProperty(k)) {
            target[k] = source[k];
        }
    }
}
copyProperties(ttsignal, CONST);

function inherits(target, source) {
  for (var k in source.prototype)
    target.prototype[k] = source.prototype[k];
}
inherits(ttsignal.Connector, EventEmitter);
inherits(ttsignal.Connection, EventEmitter);
inherits(ttsignal.Server, EventEmitter);
inherits(ttsignal.ServerConnection, EventEmitter);


/*******************************************************************************
* @class ttsignal.Connector
*******************************************************************************/

/**
 * @method _internalCallback
 * @private
 * 
 * 
 **/
ttsignal.Connector.prototype._internalCallback = function(type, __arg2, __arg3){
    switch(type){
        case 'close':
            this.emit('close');
            break;
    }
}

/**
 * 为特定事件添加一个监听处理程序.
 * 当前支持除'_error/_result/onStatus/error/close'等事件之外的 
 * 任何自定义事件.上述事件为内部保留事件，用户不应定义与这些事件同名的
 * 远程调用函数名。与用户自定义事件相对应的事件处理函数的参数列表与远端
 * 调用时传入的参数列表一致。
 *
 * @method on
 * @public
 * @async
 * @param event_name {String} 要订阅的事件名称
 * @param callback {function} 事件处理函数
 * @example
 *     connector.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 预览关闭事件及其事件监听处理程序.
 *
 * @event close
 * @example
 *     connector.on('close', function(){
 *         // do something
 *     });
 **/

/**
 * 错误事件及其事件监听处理程序.
 * 如果添加'error'事件监听函数，则当出现内部错误时回调函数将被调用。
 *
 * @event error
 * @param error_msg {Error} 错误信息
 * @example
 *     connector.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 建立连接.
 *
 * @method createConnection
 * @public
 * @param config {Object} configuration.
 * @example
 *     press.createConnection(config);
 **/
ttsignal.Connector.prototype.createConnection = function(config){
    if (typeof(config) != 'object'){
        throw Error('Invalid args value.');
    }
    var default_config = {
        c_cong_ctl : CONST.TTS_CC_BBR2
    }
    copyProperties(default_config, config);
    let conn = this.__createConnection__(default_config);
    return conn;
}

/**
 * 关闭。
 *
 * @method close
 * @public
 * @sync
 * @example
 *     connector.close();
 **/
ttsignal.Connector.prototype.close = function(){
    this.__close__();
}


/*******************************************************************************
* @class ttsignal.Connection
*******************************************************************************/

/**
 * @method _internalCallback
 * @private
 * 
 * 
 **/
ttsignal.Connection.prototype._internalCallback = function(type, __arg2, __arg3, __arg4, __arg5){
    let self = this;
    switch(type){
        case 'handshakeFinished':
            this.streams = {};
            this.emit('handshakeFinished');
            break;
        case 'streamCreated':
            if (this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = new ttsignal.Stream(this, __arg2);
            this.streams[__arg2] = stream;
            this.emit('streamCreated', stream);
            break;
        case 'streamClosed':
            if (!this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = this.streams[__arg2];
            delete this.streams[__arg2];
            stream.onClose();
            break;
        case 'streamDataAcked':
            if (!this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = this.streams[__arg2];
            stream.onDataAcked(__arg3, __arg4, __arg5);
            break;
        case 'streamDataSent':
            if (!this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = this.streams[__arg2];
            stream.onDataSent(__arg3, __arg4);
            break;
        case 'command':
            if (!this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = this.streams[__arg2];
            stream.onCommand(__arg3);
            break;
        case 'data':
            if (!this.streams.hasOwnProperty(__arg2)) {
                break;
            }
            var stream = this.streams[__arg2];
            stream.onData(__arg3, __arg4);
            break;
        case 'restart':
            this.emit('restart', __arg2, __arg3);
            break;
        case 'close':
            this.emit('close', __arg2);
            break;
    }
}

/**
 * 为特定事件添加一个监听处理程序.
 * 当前支持除'_error/_result/onStatus/error/close'等事件之外的 
 * 任何自定义事件.上述事件为内部保留事件，用户不应定义与这些事件同名的
 * 远程调用函数名。与用户自定义事件相对应的事件处理函数的参数列表与远端
 * 调用时传入的参数列表一致。
 *
 * @method on
 * @public
 * @async
 * @param event_name {String} 要订阅的事件名称
 * @param callback {function} 事件处理函数
 * @example
 *     conn.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 连接关闭事件及其事件监听处理程序.
 *
 * @event close
 * @example
 *     conn.on('close', function(){
 *         // do something
 *     });
 **/

/**
 * 底层socket重开事件及其事件监听处理程序.
 *
 * @event restart
 * @example
 *     conn.on('restart', function(err, address){
 *         // do something
 *     });
 **/

/**
 * 错误事件及其事件监听处理程序.
 * 如果添加'error'事件监听函数，则当出现内部错误时回调函数将被调用。
 *
 * @event error
 * @param error_msg {Error} 错误信息
 * @example
 *     conn.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 建立连接.
 *
 * @method connect
 * @public
 * @param url {String|Number} url string.
 * @param args {String} connect message with JSON.stringify processed.
 * @param callback {Function} connect result callback.
 * @example
 *     conn.connect(url, args, (err, result)=>{
 *         console.log(result);
 *     });
 **/
ttsignal.Connection.prototype.connect = function(url, args, timeoutInMs, callback){
    if (typeof(url) != 'string'){
        throw Error('Invalid url value.');
    }
    if (typeof(callback) != 'function'){
        throw Error('Invalid callback value.');
    }
    this.__connect__(url, JSON.stringify(args), timeoutInMs, (err, response)=>{
        if (typeof response == 'string') {
            try {
                props = JSON.parse(response);
                callback(err, props);
            } catch (e) {
                callback(err, response);
            }
        } else {
            callback(err, null);
        }
    });
}

/**
 * 重新创建底层socket。
 *
 * @method restart
 * @public
 * @sync
 * @example
 *     conn.restart();
 **/
ttsignal.Connection.prototype.restart = function(){
    this.__restart__();
}

/**
 * 关闭。
 *
 * @method close
 * @public
 * @sync
 * @example
 *     conn.close();
 **/
ttsignal.Connection.prototype.close = function(){
    this.__close__();
}

/*******************************************************************************
* @class ttsignal.Stream
*******************************************************************************/

class Stream extends EventEmitter { 
    constructor(conn, streamId){
        super();
        this.conn = conn;
        this.streamId = streamId;
        this.nextTransId = 2;
        this.stubs = {};
        this.dataStreams = {};
    }
    onCommand(cmdStr){ 
        let self = this;
        try {
            let cmd = JSON.parse(cmdStr)
            if (typeof(cmd) != 'object') {
                throw Error('Invalid command value.' + __arg2);
            }
            if (!cmd.hasOwnProperty('name')){
                throw Error('Invalid command value, MUST have "name" property.' + __arg2);
            }
            if (cmd.name == '_error' || cmd.name == '_result' || cmd.name == 'onStatus'){
                if (self.stubs.hasOwnProperty(cmd.transId)) {
                    self.stubs[cmd.transId](cmd);
                    let keepStub = cmd.name == 'onStatus'
                    if (!keepStub) {
                        delete self.stubs[cmd.transId];
                    }
                }
            } else {
                if (cmd.hasOwnProperty('transId')) {
                    cmd.echoResult = function(result){
                        self.sendCommand({
                            name: '_result',
                            transId: cmd.transId,
                            result: result
                        });
                    }
                    cmd.echoError = function(error){
                        self.sendCommand({
                            name: '_error',
                            transId: cmd.transId,
                            error: error
                        });
                    }
                } else {
                    cmd.echoResult = ()=>{};
                    cmd.echoError = ()=>{};
                }
                self.emit(cmd.name, cmd);
            }
        } catch (error) {
            console.error('parse command failed', error);
            return;
        }
    }
    onData(data, transId){ 
        if (transId && this.dataStreams.hasOwnProperty(transId)) {
            let ds = this.dataStreams[transId];
            if (data.length === 0) {
                delete this.dataStreams[transId];
                ds.emit('end');
            } else {
                ds.emit('data', data);
            }
            return;
        }
        this.emit('data', data);
    }

    onClose(){ 
        for (let tid in this.dataStreams) {
            this.dataStreams[tid].emit('end');
        }
        this.dataStreams = {};
        this.emit('close');
    }

    /** QUIC 发送侧：数据确认回调；emit `dataAcked`(ackDelayTimeµs, ackedBytes, inflightBytes)。 */
    onDataAcked(ackDelayTime, ackedBytes, inflightBytes){
        this.emit('dataAcked', ackDelayTime, ackedBytes, inflightBytes);
    }

    /** QUIC 发送侧：数据已从待发队列写入传输；emit `dataSent`(nTransId, size)。putFile 返回的 ws 仅转发本 transId 为 `dataSent`(size)。 */
    onDataSent(nTransId, size){
        this.emit('dataSent', nTransId, size);
    }

    /**
     * 发送命令.
     *
     * @method sendCommand
     * @public
     * @param cmd {Object} command to send.
     * @param callback {Function} callback function.
     * @example
     *     conn.sendCommand(cmd);
     **/
    sendCommand(cmd, callback){
        if (typeof(cmd) != 'object') {
            throw Error('Invalid command value.');
        }
        if (!cmd.hasOwnProperty('name')){
            throw Error('Invalid command value, MUST have "name" property.');
        }
        if (typeof callback === 'function') {
            cmd.transId = this.nextTransId++;
            this.stubs[cmd.transId] = callback;
        }
        let msg = JSON.stringify(cmd);
        let buf = Buffer.from(msg, 'utf8');
        this.conn.__sendPacket__(CONST.TTS_TYPE_COMMAND, (new Date).getTime(), 
            cmd.transId, this.streamId, buf);
    }

    /**
     * 发送数据.
     *
     * @method sendData
     * @public
     * @param timestamp {Number} timestamp.
     * @param data {Buffer} data to send.
     * @example
     *     conn.sendData(data);
     **/
    sendData(data){
        if (data instanceof Buffer == false) {
            throw Error('Invalid data type, MUST be Buffer.');
        }
        this.conn.__sendPacket__(CONST.TTS_TYPE_MESSAGE, (new Date).getTime(), 
            0, this.streamId, data);
    }

    /**
     * 请求文件（数据流下载）。
     *
     * @method getFile
     * @public
     * @param req {Object} 请求参数，如 {path:"index.html"}
     * @param callback {Function} 命令响应回调，透传给底层 __sendPacket__
     * @return {EventEmitter} 数据流对象，支持 'data' 和 'end' 事件
     * @example
     *     let ds = stream.getFile({path:"index.html"}, (cmd)=>{
     *         console.log('response:', cmd);
     *     });
     *     ds.on('data', (chunk) => { ... });
     *     ds.on('end', () => { ... });
     **/
    getFile(args, callback){
        if (typeof(args) != 'object') {
            throw Error('Invalid request value.');
        }
        if (!args.hasOwnProperty('path')) {
            throw Error('Invalid request value, MUST have "path" property.');
        }
        args.method = 'GET';
        let req = {
            name: 'staticFile',
            props: args
        };
        let self = this;
        let transId = this.nextTransId++;
        req.transId = transId;
        if (typeof callback === 'function') {
            this.stubs[transId] = callback;
        }
        let ds = new EventEmitter();
        ds.once('end', () => { delete self.dataStreams[transId]; });
        this.dataStreams[transId] = ds;
        let msg = JSON.stringify(req);
        let buf = Buffer.from(msg, 'utf8');
        this.conn.__sendPacket__(CONST.TTS_TYPE_COMMAND, (new Date).getTime(),
            transId, this.streamId, buf);
        return ds;
    }

    /**
     * 上传文件（数据流上传）。
     *
     * @method putFile
     * @public
     * @param req {Object} 请求参数，如 {path:"upload.bin"}
     * @param callback {Function} 命令响应回调
     * @return {events.EventEmitter} 写入流：write(data)、end()，并可 on('dataSent', (size)=>{})（仅本 putFile 事务）。
     * @example
     *     let ws = stream.putFile({path:"upload.bin"}, (cmd)=>{
     *         console.log('response:', cmd);
     *     });
     *     ws.on('dataSent', (size) => {});
     *     ws.write(chunk1);
     *     ws.write(chunk2);
     *     ws.end();
     **/
    putFile(args, callback){
        if (typeof(args) != 'object') {
            throw Error('Invalid request value.');
        }
        if (!args.hasOwnProperty('path')) {
            throw Error('Invalid request value, MUST have "path" property.');
        }
        if (!args.hasOwnProperty('size')) {
            throw Error('Invalid request value, MUST have "path" property.');
        }
        args.method = 'PUT';
        let req = {
            name: 'staticFile',
            props: args
        };
        let self = this;
        let transId = this.nextTransId++;
        req.transId = transId;
        if (typeof callback === 'function') {
            this.stubs[transId] = callback;
        }
        let msg = JSON.stringify(req);
        let buf = Buffer.from(msg, 'utf8');
        this.conn.__sendPacket__(CONST.TTS_TYPE_COMMAND, (new Date).getTime(),
            transId, this.streamId, buf);
        let ended = false;
        let ws = new EventEmitter();
        let onDataSent = (nTransId, size) => {
            if (nTransId == transId) {
                ws.emit('dataSent', size);
            }
        }
        this.on('dataSent', onDataSent);
        ws.write = (data) => {
            if (ended) {
                throw Error('Write after end.');
            }
            if (data instanceof Buffer == false) {
                throw Error('Invalid data type, MUST be Buffer.');
            }
            self.conn.__sendPacket__(CONST.TTS_TYPE_MESSAGE, (new Date).getTime(),
                transId, self.streamId, data);
        }
        ws.end = () => {
            if (ended) return;
            ended = true;
            self.conn.__sendPacket__(CONST.TTS_TYPE_MESSAGE, (new Date).getTime(),
                transId, self.streamId, Buffer.alloc(0));
            self.removeListener('dataSent', onDataSent);
        }
        return ws;
    }

    /**
     * 请求文件（数据流下载）。
     *
     * @method getLogFile
     * @public
     * @param req {Object} 请求参数，如 {path:"room_name"}
     * @param callback {Function} 命令响应回调，透传给底层 __sendPacket__
     * @return {EventEmitter} 数据流对象，支持 'data' 和 'end' 事件
     * @example
     *     let ds = stream.getLogFile({path:"room_name"}, (cmd)=>{
     *         console.log('response:', cmd);
     *     });
     *     ds.on('data', (chunk) => { ... });
     *     ds.on('end', () => { ... });
     **/
    getLogFile(args, callback){
        if (typeof(args) != 'object') {
            throw Error('Invalid request value.');
        }
        if (!args.hasOwnProperty('roomId')) {
            throw Error('Invalid request value, MUST have "roomId" property.');
        }
        args.method = 'GET';
        let req = {
            name: 'logFile',
            props: args
        };
        let self = this;
        let transId = this.nextTransId++;
        req.transId = transId;
        if (typeof callback === 'function') {
            this.stubs[transId] = callback;
        }
        let ds = new EventEmitter();
        ds.once('end', () => { delete self.dataStreams[transId]; });
        this.dataStreams[transId] = ds;
        let msg = JSON.stringify(req);
        let buf = Buffer.from(msg, 'utf8');
        this.conn.__sendPacket__(CONST.TTS_TYPE_COMMAND, (new Date).getTime(),
            transId, this.streamId, buf);
        return ds;
    }

    /**
     * 关闭。
     *
     * @method close
     * @public
     * @sync
     * @example
     *     conn.close();
     **/
    close(){
        this.conn.__closeStream__(this.streamId);
    }
}
ttsignal.Stream = Stream;

/*******************************************************************************
* @class ttsignal.Server
*******************************************************************************/

/**
 * @method _internalCallback
 * @private
 * 
 * 
 **/
ttsignal.Server.prototype._internalCallback = function(type, __arg2, __arg3){
    switch(type){
        case 'connection':
            if (__arg2 instanceof ttsignal.ServerConnection) {
                let conn = __arg2;
                conn.nextTransId = 2;
                conn.stubs = {};
                this.emit('connection', conn);
            }
            break;
        case 'close':
            this.emit('close');
            break;
    }
}

/**
 * 为特定事件添加一个监听处理程序.
 * 当前支持除'_error/_result/onStatus/error/close'等事件之外的 
 * 任何自定义事件.上述事件为内部保留事件，用户不应定义与这些事件同名的
 * 远程调用函数名。与用户自定义事件相对应的事件处理函数的参数列表与远端
 * 调用时传入的参数列表一致。
 *
 * @method on
 * @public
 * @async
 * @param event_name {String} 要订阅的事件名称
 * @param callback {function} 事件处理函数
 * @example
 *     connector.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 预览关闭事件及其事件监听处理程序.
 *
 * @event close
 * @example
 *     connector.on('close', function(){
 *         // do something
 *     });
 **/

/**
 * 错误事件及其事件监听处理程序.
 * 如果添加'error'事件监听函数，则当出现内部错误时回调函数将被调用。
 *
 * @event error
 * @param error_msg {Error} 错误信息
 * @example
 *     connector.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 开始工作。
 *
 * @method start
 * @public
 * @sync
 * @example
 *     connector.start();
 **/
ttsignal.Server.prototype.start = function(){
    this.__start__();
}

/**
 * 关闭服务器。
 *
 * @method close
 * @public
 * @sync
 * @example
 *     connector.close();
 **/
ttsignal.Server.prototype.close = function(){
    this.__close__();
}


/*******************************************************************************
* @class ttsignal.ServerConnection
*******************************************************************************/

/**
 * @method _internalCallback
 * @private
 * 
 * 
 **/
ttsignal.ServerConnection.prototype._internalCallback = function(type, __arg2, __arg3){
    var self = this;
    switch(type){
        case 'handshakeFinished':
            this.emit('handshakeFinished');
            break;
        case 'connect':
            try {
                props = JSON.parse(__arg2);
                this.emit('connect', props);
            } catch (e) {
                this.emit('connect', e);
            }
            break;
        case 'command':
            try {
                cmd = JSON.parse(__arg2);
                if (typeof(cmd) != 'object') {
                    throw Error('Invalid command value.' + __arg2);
                }
                if (!cmd.hasOwnProperty('name')){
                    throw Error('Invalid command value, MUST have "name" property.' + __arg2);
                }
                if (cmd.name == '_error' || cmd.name == '_result'){
                    if (self.stubs.hasOwnProperty(cmd.transId)) {
                        self.stubs[cmd.transId](cmd);
                        delete self.stubs[cmd.transId];
                    }
                } else {
                    if (cmd.hasOwnProperty('transId')) {
                        cmd.echoResult = function(result){
                            self.sendCommand({
                                name: '_result',
                                transId: cmd.transId,
                                result: result
                            });
                        }
                        cmd.echoError = function(error){
                            self.sendCommand({
                                name: '_error',
                                transId: cmd.transId,
                                error: error
                            });
                        }
                    } else {
                        cmd.echoResult = ()=>{};
                        cmd.echoError = ()=>{};
                    }
                    self.emit(cmd.name, cmd);
                }
            } catch (error) {
                console.error('parse command failed', error);
                return;
            }
            break;
        case 'data':
            this.emit('data', __arg2);
            break;
        case 'close':
            this.emit('close', __arg2);
            break;
    }
}

/**
 * 为特定事件添加一个监听处理程序.
 * 当前支持除'_error/_result/onStatus/error/close'等事件之外的 
 * 任何自定义事件.上述事件为内部保留事件，用户不应定义与这些事件同名的
 * 远程调用函数名。与用户自定义事件相对应的事件处理函数的参数列表与远端
 * 调用时传入的参数列表一致。
 *
 * @method on
 * @public
 * @async
 * @param event_name {String} 要订阅的事件名称
 * @param callback {function} 事件处理函数
 * @example
 *     conn.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 预览关闭事件及其事件监听处理程序.
 *
 * @event close
 * @example
 *     conn.on('close', function(){
 *         // do something
 *     });
 **/

/**
 * 错误事件及其事件监听处理程序.
 * 如果添加'error'事件监听函数，则当出现内部错误时回调函数将被调用。
 *
 * @event error
 * @param error_msg {Error} 错误信息
 * @example
 *     conn.on('error', function(err){
 *         if (err) {
 *             console.dir(err);
 *         }
 *     });
 **/

/**
 * 发送命令.
 *
 * @method sendCommand
 * @public
 * @param cmd {Object} command to send.
 * @param callback {Function} callback function.
 * @example
 *     conn.sendCommand(cmd, function(result){
 *         console.log(result);
 *     });
 **/
ttsignal.ServerConnection.prototype.sendCommand = function(cmd, callback){
    if (typeof(cmd) != 'object') {
        throw Error('Invalid command value.');
    }
    if (!cmd.hasOwnProperty('name')){
        throw Error('Invalid command value, MUST have "name" property.');
    }
    if (typeof callback === 'function') {
        cmd.transId = this.nextTransId++;
        this.stubs[cmd.transId] = callback;
    }
    let msg = JSON.stringify(cmd);
    let buf = Buffer.from(msg, 'utf8');
    this.__sendPacket__(CONST.TTS_TYPE_COMMAND, (new Date).getTime(), cmd.transId, 0, buf);
}

/**
 * 发送数据.
 *
 * @method sendData
 * @public
 * @param timestamp {Number} timestamp.
 * @param data {Buffer} data to send.
 * @example
 *     conn.sendData(data);
 **/
ttsignal.ServerConnection.prototype.sendData = function(data){
    if (data instanceof Buffer == false) {
        throw Error('Invalid data type, MUST be Buffer.');
    }
    this.__sendPacket__(CONST.TTS_TYPE_MESSAGE, (new Date).getTime(), 0, data);
}

/**
 * 接受连接。
 *
 * @method accept
 * @public
 * @sync
 * @example
 *     conn.accept(true, { server_param: 'server param' });
 **/
ttsignal.ServerConnection.prototype.accept = function(accept, result){
    if (typeof(accept) != 'boolean') {
        throw Error('Invalid accept value.');
    }
    this.__accept__(accept, JSON.stringify(result));
}

/**
 * 关闭。
 *
 * @method close
 * @public
 * @sync
 * @example
 *     conn.close();
 **/
ttsignal.ServerConnection.prototype.close = function(){
    this.__close__();
}

/*******************************************************************************
* @class 
*******************************************************************************/

/**
 * 创建连接器.
 *
 * @method createConnector
 * @public
 * @param config {Object} configure info.
 * @param config.alpn {String} required, ALPN string ('ttsignal' on the wire regardless).
 * @param config.ca_cert_pem {String=} optional, custom root CA in PEM format.
 * @param config.disableAutoRestart {Boolean=} optional, default `false`.
 *   When `false` (default) ttsignal starts a platform-native path-change
 *   monitor (NWPathMonitor on macOS, NETLINK_ROUTE on Linux,
 *   NotifyIpInterfaceChange on Windows) and automatically restarts the
 *   underlying UDP socket whenever the OS reports a different default
 *   interface (Wi-Fi <-> ethernet, Wi-Fi <-> wired, VPN up/down). Each
 *   auto-restart fires the existing `connection.on('restart', cb)` event,
 *   so application code does NOT need to call `connection.restart()`
 *   manually. Set `disableAutoRestart: true` for long-lived server
 *   deployments where the box never roams.
 * @param config.vpnPolicy {String=} optional, one of `'os'`,
 *   `'prefer-physical'`, `'force-physical'`. Controls whether QUIC traffic is
 *   allowed to ride a VPN / virtual interface (utun on macOS/iOS, tun on
 *   Linux, wintun/TAP on Windows).
 *
 *   - `'os'` — follow the kernel's routing decision, including VPN tunnels.
 *     No interface pin is installed at all.
 *   - `'prefer-physical'` — prefer wifi/wired/cellular. At startup, fall back
 *     to a tunnel if no physical interface exists; while running, refuse to
 *     migrate onto a tunnel (keep the current socket instead).
 *   - `'force-physical'` — physical interfaces only. Never fall back, and
 *     never undo the interface pin even when the kernel routing table says
 *     the peer is reachable only through the tunnel. Use this when the server
 *     must observe the client's real IP rather than the VPN egress IP.
 *
 *   Defaults: `'prefer-physical'` on macOS / Windows / Linux, `'os'` on iOS.
 *
 *   ⚠️ Windows behaviour change: before this option existed, Windows
 *   effectively behaved as `'os'` (GetBestInterfaceEx does no interface-type
 *   filtering). It now defaults to `'prefer-physical'` like the other desktop
 *   platforms. Pass `vpnPolicy: 'os'` explicitly to keep the old behaviour.
 *
 *   With `'force-physical'`, `connection.connect()` fails immediately with
 *   `BC_R_NO_PHYSICAL_INTERFACE` (64) when no physical interface is
 *   available — it does NOT wait for the connect timeout. Handle that error
 *   by either reconnecting with `'prefer-physical'` or telling the user to
 *   turn off their VPN.
 *
 *   Linux caveat: only `SO_BINDTODEVICE` can truly guarantee packets leave
 *   via the physical NIC, and it requires `CAP_NET_RAW` (or root). Without
 *   that capability the addon falls back to `IP_UNICAST_IF`, which is
 *   silently ignored on kernels older than 6.0.16 / 6.1.2 / 6.2 for connected
 *   UDP sockets. On such hosts under a full-tunnel proxy, `'force-physical'`
 *   guarantees only that failures are visible, not that the real IP gets
 *   through. A warning is logged when this fallback happens.
 * @param config.bypassVpn {Boolean=} **deprecated**, use `vpnPolicy` instead.
 *   Mapped as `false` -> `'os'`, `true` -> `'prefer-physical'`. When both are
 *   given, `vpnPolicy` wins and a warning is logged.
 * @param config.proxyUrl {String=} optional MASQUE CONNECT-UDP (RFC 9298)
 *   proxy. When set, every connection created from this connector tunnels its
 *   QUIC packets to the real server through an HTTP/3 CONNECT-UDP session to
 *   this proxy (the proxy must speak MASQUE connect-udp). Accepted forms:
 *   `masque://host:port`, `https://host:port`, `h3://host:port`, or a bare
 *   `host:port` (defaults to MASQUE). Port defaults to 443; IPv6 must be
 *   bracketed (`[2001:db8::1]:443`). Alias: `proxy_url`.
 * @param config.proxyHost {String=} optional, proxy host. Overrides the host
 *   parsed from `proxyUrl`; setting it alone enables the MASQUE proxy.
 *   Alias: `proxy_host`.
 * @param config.proxyPort {Number=} optional, proxy port (default 443).
 *   Alias: `proxy_port`.
 * @param config.proxySni {String=} optional, TLS SNI presented to the proxy
 *   (defaults to the proxy host). Alias: `proxy_sni`.
 * @example
 *     var connector = ttsignal.createConnector({
 *         alpn: 'ttsignal',
 *         disableAutoRestart: false,  // default
 *     });
 * @example
 *     // Route all connections through a MASQUE CONNECT-UDP proxy:
 *     var connector = ttsignal.createConnector({
 *         alpn: 'ttsignal',
 *         proxyUrl: 'masque://proxy.example.com:443',
 *     });
 **/
ttsignal.createConnector = function(config){
    if (typeof config !== 'object') {
        throw Error('Invalid config.');
    }
    if (!config.alpn){
        throw Error('Invalid alpn value.');
    }
    // 在 JS 层就对非法取值抛错，比让原生层告警后静默回落平台默认值更容易
    // 发现拼写错误——后者只会在日志里留一行 WARN，业务多半看不到。
    if (config.vpnPolicy !== undefined) {
        var VALID_VPN_POLICIES = ['os', 'prefer-physical', 'force-physical'];
        if (VALID_VPN_POLICIES.indexOf(config.vpnPolicy) === -1) {
            throw Error('Invalid vpnPolicy: ' + config.vpnPolicy +
                '. Expected one of ' + VALID_VPN_POLICIES.join(', ') + '.');
        }
    }
    if (config.caCertPem && !config.ca_cert_pem) {
        config.ca_cert_pem = config.caCertPem;
    }
    // MASQUE CONNECT-UDP proxy config: accept camelCase aliases and forward as
    // the snake_case keys the native layer reads (proxy_url/host/port/sni).
    if (config.proxyUrl && !config.proxy_url) {
        config.proxy_url = config.proxyUrl;
    }
    if (config.proxyHost && !config.proxy_host) {
        config.proxy_host = config.proxyHost;
    }
    if (config.proxyPort && !config.proxy_port) {
        config.proxy_port = config.proxyPort;
    }
    if (config.proxySni && !config.proxy_sni) {
        config.proxy_sni = config.proxySni;
    }
    return ttsignal.__createConnector__(config);
}

/**
 * 创建服务器.
 *
 * @method createServer
 * @public
 * @param config {Object} configure info.
 * @example
 *     var succeed = ttsignal.createServer(config);
 **/
ttsignal.createServer = function(config){
    if (typeof config !== 'object') {
        throw Error('Invalid config.');
    }
    if (!config.alpn){
        throw Error('Invalid alpn value.');
    }
    return ttsignal.__createServer__(config);
}

//*******************************************************************************
// HTTP / WebSocket 栈（真实 IP 出网）的胶水层
//
// 原生层导出的是带双下划线的原始接口（__createHttpConnector__ / __request__ /
// __createWsConnector__ / __connect__ ...），本节把它们包成惯用的 JS 形态。
//
//   HTTP —— 一次性请求，用 Promise
//     const http = ttsignal.createHttpConnector({ vpnPolicy: 'force-physical' });
//     const resp = await http.request({ method: 'GET', url: 'https://host/path',
//                                       headers: { authorization: token },
//                                       timeoutMs: 5000 });
//     resp.status / resp.reason / resp.headers / resp.body(Buffer)
//     resp.peerIp / resp.boundIfIndex / resp.pinMethod
//     await http.close();
//
//   WS —— 长连接，用 EventEmitter（与 SMP 的 Connection 一致）
//     const ws   = ttsignal.createWsConnector({ vpnPolicy: 'force-physical' });
//     const conn = ws.createConnection();
//     conn.on('text', (s) => {});         conn.on('data', (buf) => {});
//     conn.on('closed', (reason) => {});  conn.on('exception', (msg) => {});
//     await conn.connect('wss://host/path', 5000);
//     conn.sendText('hello'); conn.sendData(buf);
//     await conn.close();
//     await ws.close();
//
// `vpnPolicy: 'force-physical'` 的意义：绕开 VPN / 虚拟网卡，用**用户真实 IP**
// 出网（业务侧最典型的用途是请求接入点接口，避免在 VPN 环境下被调度到错误地域
// 的节点）。是否真的绑上了物理网卡，看 `resp.boundIfIndex` /
// `connect()` 的返回值 / `conn.info().boundIfIndex`：非 0 就是真的绑了，
// `pinMethod` 说明绑法（macOS/iOS 是 IP_BOUND_IF，Linux 是 SO_BINDTODEVICE），
// 不用翻日志。
//
// ------------------------------------------------------------------------------
// 一、错误对象：result / errName / errMessage 一个都不能丢
// ------------------------------------------------------------------------------
// reject 出来的就是原生层给的那个 Error，**原样透传**，不重新包装：
//   err.result      数字错误码（BCRESULT，跨语言契约值）
//   err.errName     错误码符号名，如 'BC_R_ROUTE_MISMATCH'
//   err.errMessage  原生层的现场描述 —— force-physical 失败时这段写得很细，
//                   包含对端 IP、peerClass、内核把包判给了哪块网卡、以及该怎么
//                   改配置。**排查 force-physical 只能看这一段**，通用提示给不出
//                   这些细节。
// 常用取值（见 CONST 里的 TTS_R_* 常量）：
//   64 BC_R_NO_PHYSICAL_INTERFACE  当下没有可用物理网卡
//   69 BC_R_PIN_FAILED             绑定网卡失败
//   70 BC_R_ROUTE_MISMATCH         对端从物理网卡出不去（连环回/内网即是这个）
//   71 BC_R_DNS_FAILED             DNS 失败
//   72 BC_R_TLS_VERIFY_FAILED      证书校验失败
//   73 BC_R_RESPONSE_TOO_LARGE     响应超过 maxResponseBytes（仅 HTTP）
//   74 BC_R_WS_HANDSHAKE_FAILED    WebSocket 握手失败（仅 WS）
// ⚠️ 发送失败（sendText / sendData / sendPing）抛出的 Error 只有 result，没有
// errName / errMessage —— 原生的发送接口只给错误码，不给文案。
//
// ------------------------------------------------------------------------------
// 二、监听器 / Promise 回调里抛异常
// ------------------------------------------------------------------------------
// `emit` 是**同步**调用监听器的，而这些 emit 发生在原生回调（uv_async）栈里。
// 监听器抛出的异常若原样冒回 C++，会在 env 里留下 pending exception，同一批
// uv_async 的下一个事件就会让 C++ 异常逃出事件泵 -> `libc++abi: terminating`，
// **打死整个 node 进程**。原生层已经用 DrainPendingException() 兜住了这一层
// （会报成 uncaughtException），本胶水层再兜一道：所有 emit 都走 ttEmitSafely，
// 监听器抛出的异常被就地捕获、挪到 process.nextTick 再抛出去。效果是：
//   * 进程不死，异常照常出现在 process 的 'uncaughtException' 上（不会被吞掉）；
//   * 同一批里的其余事件、以及后续事件，都照常交付。
// 同理，Promise 的 resolve / reject 之后本胶水层**不再执行任何业务代码** ——
// then/catch 里的用户代码同样是同步跑在原生回调栈上的。
//
// ------------------------------------------------------------------------------
// 三、close()
// ------------------------------------------------------------------------------
// * `connector.close()` 可以重复调，**每一次**返回的 Promise 都会 settle
//   （原生层把 close 回调收成列表，且已关闭后仍会补一次信号）。
// * `conn.close()` 在"握手成功过"的连接上等 closed 事件；对**从没握手成功过**
//   的连接（没 connect 过、connect 失败、还在连接中）立刻 resolve —— 那种连接
//   按 WSConnector.h 契约第 2 条永远不会有 closed 事件，等下去就是永远悬着。
// * 连接器关闭时，还悬着的 `conn.close()` 一并 resolve（~WSConnector 的放弃路径
//   下 closed 永远不会来）。
//
// ------------------------------------------------------------------------------
// 四、生命周期：用完**显式** close()，别指望 GC
// ------------------------------------------------------------------------------
// * HttpConnector / WsConnector 的析构会等在途回调跑完，**最多阻塞主线程
//   drainTimeoutMs**（默认 30 秒）。显式 close() 是异步等待，不占主线程。
// * 业务应当**复用连接器单例**，不要按请求建连接器：每个连接器都会装一个日志
//   appender，还各自带一份 DNS / TLS 上下文。
// * 一条 WsConnection 只能 connect 一次（第二次当场抛）。重连 = 新建连接。
// * `logFile` 与 `log_callback` 同时给时 **logFile 优先，log_callback 一条都不会
//   触发**（原生层给了 logFile 就只装文件 appender）。这里会 emitWarning 提醒。
//
// ------------------------------------------------------------------------------
// 五、hasPathMonitor
// ------------------------------------------------------------------------------
// `connector.info().hasPathMonitor` 报的是**编译期**有没有 TT_HAS_PATH_MONITOR。
// 为 false 说明这个 .node 把网卡绑定整段条件编译掉了：`prefer-physical` 会静默
// 回落系统路由（= VPN 隧道），`force-physical` 则硬失败 64。它就是为了让"构建
// 配置悄悄把绑定关掉"这类缺陷在任何跑得起来的产物上当场露馅 —— 生产前建议断言
// 它为 true。
//*******************************************************************************

const VPN_POLICIES = ['os', 'prefer-physical', 'force-physical'];

// 供业务比对 err.result 用。原生层原样透传 BCRESULT，这里只是给几个常用码起个名，
// 免得业务里到处写魔法数字。
const HTTP_WS_ERRORS = {
    TTS_R_NO_PHYSICAL_INTERFACE : 64,
    TTS_R_PIN_FAILED            : 69,
    TTS_R_ROUTE_MISMATCH        : 70,
    TTS_R_DNS_FAILED            : 71,
    TTS_R_TLS_VERIFY_FAILED     : 72,
    TTS_R_RESPONSE_TOO_LARGE    : 73,
    TTS_R_WS_HANDSHAKE_FAILED   : 74
};
copyProperties(ttsignal, HTTP_WS_ERRORS);

/**
 * 安全 emit：监听器抛出的异常挪到下一个 tick 再抛。
 *
 * emit 是同步的，而这些 emit 跑在原生回调栈上；异常原样冒回 C++ 会留下 pending
 * exception，同一批 uv_async 的下一个事件就会把整个进程打死。挪到 nextTick 抛的
 * 效果是：异常照常出现在 'uncaughtException'（不吞），但不经过 C++ 栈。
 *
 * @private
 */
function ttEmitSafely(target, type) {
    const args = Array.prototype.slice.call(arguments, 1);
    try {
        target.emit.apply(target, args);
    } catch (err) {
        process.nextTick(function () { throw err; });
    }
}

/**
 * 校验并返回一份可以交给原生层的 config。
 *
 * vpnPolicy 拼错在 JS 层就抛：原生层虽然也会抛，但在这里抛能把"合法取值"直接
 * 写进异常文案，且与 createConnector 的既有行为一致。
 *
 * @private
 */
function ttNormalizeStackConfig(config, what) {
    if (config === undefined || config === null) {
        config = {};
    }
    if (typeof config !== 'object') {
        throw Error('Invalid config for ' + what + ': expected an object.');
    }
    if (config.vpnPolicy !== undefined && config.vpnPolicy !== null) {
        if (VPN_POLICIES.indexOf(config.vpnPolicy) === -1) {
            throw Error('Invalid vpnPolicy: ' + config.vpnPolicy +
                '. Expected one of ' + VPN_POLICIES.join(', ') + '.');
        }
    }
    // logFile 与 log_callback 互斥且 logFile 优先。静默丢掉 log_callback 是很难
    // 察觉的（业务只会看到"一条日志都没来"），所以出个警告。
    if (config.logFile && typeof config.log_callback === 'function') {
        process.emitWarning(
            '[ttsignal] ' + what + ': logFile 与 log_callback 同时给了，' +
            'logFile 优先，log_callback 一条都不会触发。', 'TTSignalConfigWarning');
    }
    return config;
}

/** 原生产物不含 HTTP/WS 栈时给一句能看懂的话，而不是 "xxx is not a function"。 */
function ttRequireStack(factoryName, what) {
    if (typeof ttsignal[factoryName] !== 'function') {
        throw Error('当前 ttsignal 原生产物不含 ' + what + '（缺少 ' +
            factoryName + '）。请重新构建 addon（见 CLAUDE.md 的 Build Commands）。');
    }
}

/*******************************************************************************
* @class ttsignal.HttpConnector
*
* 一次性 HTTP/HTTPS 请求。**复用单例**，用完 await close()。
*******************************************************************************/

if (typeof ttsignal.HttpConnector === 'function') {

inherits(ttsignal.HttpConnector, EventEmitter);

/**
 * @method _internalCallback
 * @private
 **/
ttsignal.HttpConnector.prototype._internalCallback = function (type) {
    switch (type) {
    case 'close':
        // 原生层每次 __close__ 都会给一次信号（已关闭时补投一个事件），但对外的
        // 'close' 事件按 Node 惯例只发一次。
        if (this.__closeEmitted) {
            break;
        }
        this.__closeEmitted = true;
        ttEmitSafely(this, 'close');
        break;
    }
};

/**
 * 发起一次请求。
 *
 * @method request
 * @public
 * @async
 * @param options {Object} 请求参数
 * @param options.url {String} 必填，http:// 或 https://
 * @param options.method {String=} 默认 GET
 * @param options.headers {Object=} 头，值必须是字符串；含 CRLF 会被当注入拦下
 * @param options.body {String|Buffer=} 请求体
 * @param options.timeoutMs {Number=} 覆盖 DNS + connect + TLS + 收响应的总预算
 * @param options.resolvedIp {String=} 跳过 DNS，直接连这个 IP
 * @return {Promise<Object>} resolve：{ status, reason, headers, body(Buffer),
 *   peerIp, boundIfIndex, pinMethod }；4xx / 5xx 也算正常响应（resolve）。
 *   reject：原生 Error，带 result / errName / errMessage。
 *
 * ⚠️ 参数用错（缺 url、类型不对）同样是 **reject**，不是同步抛 —— 一个既可能
 * 同步抛又可能异步 reject 的 async 接口是经典的坑。那类错误没有 result 字段。
 * ⚠️ resolve 之后不要在 then 里做耗时的事：then 的回调是同步跑在原生回调栈上的。
 *
 * @example
 *     const resp = await http.request({ url: 'https://ipinfo.io/ip' });
 *     console.log(resp.status, resp.body.toString('utf8'), resp.boundIfIndex);
 **/
ttsignal.HttpConnector.prototype.request = function (options) {
    const self = this;
    return new Promise(function (resolve, reject) {
        // __request__ 的同步抛（参数校验）会被 Promise 构造器转成 reject。
        self.__request__(options, function (err, resp) {
            if (err) {
                reject(err);        // 原样透传：result / errName / errMessage 都在
                return;
            }
            resolve(resp);
        });
        // ⚠️ resolve / reject 之后不要再写业务代码。
    });
};

/**
 * 关闭连接器。可以重复调，每一次的 Promise 都会 settle。
 *
 * 在途请求会被 reject（不会悬着）。**用完一定要显式调**：靠 GC 的话析构会在主
 * 线程上等在途回调收尾，最多阻塞 drainTimeoutMs。
 *
 * @method close
 * @public
 * @async
 * @return {Promise<void>}
 **/
ttsignal.HttpConnector.prototype.close = function () {
    const self = this;
    return new Promise(function (resolve) {
        self.__close__(resolve);
    });
};

/**
 * 生效配置与运行期状态的只读快照。**形状恒定**，关闭之后字段一个不少。
 *
 * @method info
 * @public
 * @return {Object} { closed, vpnPolicy, pendingRequests, maxResponseBytes,
 *   drainTimeoutMs, maxIdleConnections, idleTimeoutMs, hasPathMonitor }
 *   hasPathMonitor 为 false = 这个产物把网卡绑定条件编译掉了，见本节文档第五条。
 **/
ttsignal.HttpConnector.prototype.info = function () {
    return this.__info__();
};

/**
 * 运行期调整 keep-alive 空闲连接的存活上限。
 *
 * 0 = 不设本地超时，完全听服务端的。上限 24 小时（越界夹住并打 WARN）。
 * 连接器已关闭时静默忽略（与 close() 的幂等语义一致）。
 *
 * ⚠️ 只影响**此后**归还入池的连接：已经躺在池里的那些仍按各自入池时的时长
 * 计时。要让新值立刻对全部连接生效，改完再重建连接器。
 *
 * @method setIdleTimeoutMs
 * @public
 * @param ms {Number} 毫秒，0 .. 4294967295。非数字抛 TypeError，越界抛 RangeError
 * @example
 *     const http = ttsignal.createHttpConnector();
 *     http.setIdleTimeoutMs(5 * 60 * 1000);   // 空闲 5 分钟就收
 *     console.log(http.info().idleTimeoutMs); // 300000
 **/
ttsignal.HttpConnector.prototype.setIdleTimeoutMs = function (ms) {
    this.__setIdleTimeoutMs__(ms);
};

}   // typeof ttsignal.HttpConnector === 'function'

/*******************************************************************************
* @class ttsignal.WsConnector / ttsignal.WsConnection
*******************************************************************************/

if (typeof ttsignal.WsConnector === 'function' &&
    typeof ttsignal.WsConnection === 'function') {

inherits(ttsignal.WsConnector, EventEmitter);
inherits(ttsignal.WsConnection, EventEmitter);

/**
 * 把还悬着的 conn.close() Promise 收掉。重复 resolve 是无害的（Promise 幂等），
 * 所以同一个 resolver 同时登记在连接和连接器两处也没问题。
 *
 * @private
 */
function ttDrainCloseWaiters(conn) {
    const waiters = conn.__closeWaiters;
    if (!waiters || waiters.length === 0) {
        return;
    }
    conn.__closeWaiters = [];
    const connector = conn.__connector;
    for (let i = 0; i < waiters.length; i++) {
        if (connector && connector.__connCloseWaiters) {
            connector.__connCloseWaiters.delete(waiters[i]);
        }
        waiters[i]();
    }
}

/**
 * @method _internalCallback
 * @private
 **/
ttsignal.WsConnector.prototype._internalCallback = function (type, arg) {
    switch (type) {
    case 'close':
        // 连接器收尾了 -> 不会再有任何事件进来。还悬着的 conn.close() 必须在这里
        // 收掉：~WSConnector 的放弃路径下 closed 事件永远不会来。
        if (this.__connCloseWaiters) {
            const waiters = Array.from(this.__connCloseWaiters);
            this.__connCloseWaiters.clear();
            for (let i = 0; i < waiters.length; i++) {
                waiters[i]();
            }
        }
        if (!this.__closeEmitted) {
            this.__closeEmitted = true;
            ttEmitSafely(this, 'close');
        }
        break;
    case 'exception':
        ttEmitSafely(this, 'exception', arg);
        break;
    }
};

/**
 * 建一条连接（还没发起，需要再调 conn.connect()）。
 *
 * @method createConnection
 * @public
 * @param config {Object=} 连接级配置，逐键覆盖连接器的默认值。常用键：
 *   vpnPolicy / connectTimeoutMs / pingIntervalMs / idleTimeoutMs / maxFrameBytes /
 *   caCerts / spkiPin / insecureSkipVerify / dnsServers / dnsTimeoutMs
 * @return {ttsignal.WsConnection}
 **/
ttsignal.WsConnector.prototype.createConnection = function (config) {
    const conn = this.__createConnection__(
        config === undefined || config === null ? undefined :
            ttNormalizeStackConfig(config, 'createConnection'));
    // 连接反指连接器，用来在连接器关闭时兜底 settle 还悬着的 conn.close()。
    // 方向是单向的（连接 -> 连接器）：连接器只存 resolver 闭包，**不存连接对象
    // 本身**，免得连接器反过来强引用它的连接、把 GC 挡住。
    Object.defineProperty(conn, '__connector', {
        value: this, enumerable: false, writable: false, configurable: true
    });
    return conn;
};

/**
 * 关闭连接器。可以重复调，每一次的 Promise 都会 settle。
 *
 * 在途连接会被 settle（connect 的 Promise reject），已连上的连接会收到 closed。
 * **用完一定要显式调**：靠 GC 的话析构会在主线程上等在途回调收尾，最多阻塞
 * drainTimeoutMs。
 *
 * @method close
 * @public
 * @async
 * @return {Promise<void>}
 **/
ttsignal.WsConnector.prototype.close = function () {
    const self = this;
    return new Promise(function (resolve) {
        self.__close__(resolve);
    });
};

/**
 * 生效配置与运行期状态的只读快照。**形状恒定**，关闭之后字段一个不少。
 *
 * @method info
 * @public
 * @return {Object} { closed, connections, vpnPolicy, drainTimeoutMs,
 *   hasPathMonitor }
 **/
ttsignal.WsConnector.prototype.info = function () {
    return this.__info__();
};

/**
 * 连接统计。关闭之后报的是最后一次快照（关完正是最想看统计的时候）。
 *
 * @method stats
 * @public
 * @return {Object} { allocated_conn_size, active_conn_size,
 *   handshake_failed_size, ... }
 **/
ttsignal.WsConnector.prototype.stats = function () {
    return this.__stats__();
};

/**
 * @method _internalCallback
 * @private
 **/
ttsignal.WsConnection.prototype._internalCallback = function (type, arg) {
    switch (type) {
    case 'text':
        ttEmitSafely(this, 'text', arg);
        break;
    case 'data':
        ttEmitSafely(this, 'data', arg);
        break;
    case 'closed':
        this.__closedSeen = true;
        // 先 emit 再收 Promise：业务的 closed 监听器应当在 await close() 返回之前
        // 跑完（emit 是同步的，close() 的 Promise 只能在微任务里继续）。
        ttEmitSafely(this, 'closed', arg);
        ttDrainCloseWaiters(this);
        break;
    case 'exception':
        ttEmitSafely(this, 'exception', arg);
        break;
    }
};

/**
 * 发起连接。**一条连接只能 connect 一次**（第二次当场抛），重连 = 新建连接。
 *
 * 没有 connectResult 事件 —— 那是"恰好一次"的语义，Promise 才是自然形态。
 *
 * @method connect
 * @public
 * @async
 * @param url {String} ws:// 或 wss://
 * @param timeoutMs {Number=} DNS + connect + TLS + 握手响应的总预算，0 / 不给则
 *   用配置里的 connectTimeoutMs（默认 10 秒）
 * @return {Promise<Object>} resolve：{ headers, peerIp, boundIfIndex, pinMethod }。
 *   reject：原生 Error，带 result / errName / errMessage。
 *
 * ⚠️ 握手失败与"连上之后被关闭"是可区分的：握手失败**只有** reject，**不会**有
 * closed 事件；握手成功过的连接一定先 resolve、之后某个时刻才 closed。业务的重连
 * 逻辑可以据此分支。
 *
 * @example
 *     const ok = await conn.connect('wss://sfu.example.com/signal', 5000);
 *     console.log(ok.peerIp, ok.boundIfIndex, ok.pinMethod);
 **/
ttsignal.WsConnection.prototype.connect = function (url, timeoutMs) {
    const self = this;
    return new Promise(function (resolve, reject) {
        // __connect__ 的同步抛（参数校验 / 重复 connect）会被 Promise 构造器转成
        // reject —— 与 HttpConnector.request 一致。
        self.__connect__(url, timeoutMs === undefined || timeoutMs === null ?
                         0 : timeoutMs, function (err, info) {
            if (err) {
                reject(err);        // 原样透传：result / errName / errMessage 都在
                return;
            }
            resolve(info);
        });
        // ⚠️ resolve / reject 之后不要再写业务代码。
    });
};

/**
 * 发送一个文本帧。
 *
 * @method sendText
 * @public
 * @sync
 * @param text {String}
 * @throws {Error} 发送失败时抛出，err.result 是原生错误码（没有 errName /
 *   errMessage —— 原生发送接口只给码）。**刻意抛而不是返回错误码**：返回码没人看
 *   就是静默丢消息，而 WebSocket 标准里对未 OPEN 的连接 send 也是抛的。想避开
 *   try/catch 就订阅 'closed' 事件、或看 conn.info().closed。
 **/
ttsignal.WsConnection.prototype.sendText = function (text) {
    ttThrowIfSendFailed(this.__sendText__(text), 'sendText');
};

/**
 * 发送一个二进制帧。
 *
 * @method sendData
 * @public
 * @sync
 * @param data {Buffer}
 * @throws {Error} 同 sendText
 **/
ttsignal.WsConnection.prototype.sendData = function (data) {
    ttThrowIfSendFailed(this.__sendData__(data), 'sendData');
};

/**
 * 发一个 ping（对端的 pong 由原生层静默吃掉，不产生事件）。
 * 一般不用手动调：连接级 pingIntervalMs 会自动保活。
 *
 * @method sendPing
 * @public
 * @sync
 * @throws {Error} 同 sendText
 **/
ttsignal.WsConnection.prototype.sendPing = function () {
    ttThrowIfSendFailed(this.__sendPing__(), 'sendPing');
};

/** @private */
function ttThrowIfSendFailed(result, what) {
    if (result === 0) {
        return;
    }
    const err = Error(what + ' failed: result=' + result +
        '（连接可能已关闭或还没连上；订阅 closed 事件或看 conn.info()）');
    err.result = result;
    throw err;
}

/**
 * 关闭这条连接。幂等；可以重复调，每一次的 Promise 都会 settle。
 *
 * @method close
 * @public
 * @async
 * @param reason {Number=} 可选的关闭原因码（BCRESULT），给了非法值按正常关闭处理
 * @return {Promise<void>} 握手成功过的连接：closed 事件到达时 resolve。从没握手
 *   成功过的连接（没 connect 过 / connect 失败 / 还在连接中）：立刻 resolve ——
 *   那种连接按契约永远不会有 closed 事件，等下去就是永远悬着。连接器被关闭时，
 *   还悬着的 Promise 一并 resolve。
 **/
ttsignal.WsConnection.prototype.close = function (reason) {
    const info = this.__info__();
    this.__close__(reason);
    if (this.__closedSeen || info.closed || !info.upgraded) {
        return Promise.resolve();
    }
    const self = this;
    return new Promise(function (resolve) {
        if (!self.__closeWaiters) {
            self.__closeWaiters = [];
        }
        self.__closeWaiters.push(resolve);
        // 连接器关闭时的兜底（放弃路径下 closed 永远不来）。只登记 resolver，
        // 不登记连接对象本身。
        const connector = self.__connector;
        if (connector) {
            if (!connector.__connCloseWaiters) {
                connector.__connCloseWaiters = new Set();
            }
            connector.__connCloseWaiters.add(resolve);
        }
    });
};

/**
 * 这条连接的只读快照。**形状恒定**，关闭之后字段一个不少。
 *
 * @method info
 * @public
 * @return {Object} { id, attached, connecting, upgraded, closed, peerIp,
 *   boundIfIndex, pinMethod, lastError, hasPathMonitor }
 *   lastError 是原生给的最后一段失败现场描述（connectResult 的 err 只出现那一次，
 *   事后复查就看这里）。
 **/
ttsignal.WsConnection.prototype.info = function () {
    return this.__info__();
};

}   // typeof ttsignal.WsConnector === 'function' && ...

/**
 * 创建 HTTP 连接器。**复用单例**，不要按请求建。
 *
 * @method createHttpConnector
 * @public
 * @param config {Object=} 配置
 * @param config.vpnPolicy {String=} 'os' | 'prefer-physical' | 'force-physical'。
 *   默认按平台（macOS / Windows / Linux 是 'prefer-physical'）。
 *   'force-physical' = 只走物理网卡、绝不回落，用户真实 IP 出网；出不去就失败，
 *   错误码与现场描述见本节文档第一条。
 * @param config.logLevel {Number=} 1 DEBUG ... 5 FATAL
 * @param config.logFile {String=} 日志文件；给了它 log_callback 就不会触发
 * @param config.log_callback {Function=} (level, msg) => {}
 * @param config.caCerts {String=} 额外信任的 PEM
 * @param config.spkiPin {String=} SPKI pin
 * @param config.insecureSkipVerify {Boolean=} 跳过证书校验（别在生产用）
 * @param config.maxResponseBytes {Number=} 响应上限，超了报 73
 * @param config.drainTimeoutMs {Number=} 关闭时等在途请求的上限
 * @param config.dnsServers {String=} / @param config.dnsTimeoutMs {Number=}
 * @param config.idleTimeoutMs {Number=} 空闲连接在池里躺多久没人用就自行关掉，
 *   默认 3600000（60 分钟），上限 86400000（24 小时）。每次归还重新计时。
 *   **0 = 不设本地超时**，完全听服务端的。运行期可用 `setIdleTimeoutMs()` 改。
 *   它是一道兜底：正常情况下服务端自己的 keep-alive 超时会先把连接收走，这里
 *   只负责挡住「永不主动关连接」的服务端把 fd 长期占住。
 * @param config.maxIdleConnections {Number=} keep-alive 连接池里每个目标
 *   （host+port+tls+网卡策略）最多囤几条空闲连接，默认 4，上限 64。
 *   **0 = 关掉复用**，退回「一次请求一条连接」并显式发 Connection: close。
 *   默认开着复用：同一个 connector 连发多条请求时，第二条起省掉 TCP 握手和
 *   整套 TLS 握手（实测同一 https 端点 280ms -> 95ms）。要吃到这个收益就得
 *   **复用同一个 connector**，每条请求现建一个等于没开。
 * @return {ttsignal.HttpConnector}
 * @example
 *     const http = ttsignal.createHttpConnector({ vpnPolicy: 'force-physical' });
 *     try {
 *         const resp = await http.request({ url: 'https://ipinfo.io/ip' });
 *     } finally {
 *         await http.close();
 *     }
 **/
ttsignal.createHttpConnector = function (config) {
    ttRequireStack('__createHttpConnector__', 'HTTP 栈');
    return ttsignal.__createHttpConnector__(
        ttNormalizeStackConfig(config, 'createHttpConnector'));
};

/**
 * 创建 WebSocket 连接器。**复用单例**，一个连接器可以开多条连接。
 *
 * @method createWsConnector
 * @public
 * @param config {Object=} 配置。除 createHttpConnector 那几项（vpnPolicy /
 *   logLevel / logFile / log_callback / caCerts / spkiPin / insecureSkipVerify /
 *   drainTimeoutMs / dnsServers / dnsTimeoutMs）之外还有：
 *   connectTimeoutMs / pingIntervalMs / idleTimeoutMs / maxFrameBytes /
 *   clientCertFile / clientKeyFile / clientKeyPassword。
 *   这些键在 createConnection(config) 里可以逐条覆盖。
 * @return {ttsignal.WsConnector}
 * @example
 *     const ws = ttsignal.createWsConnector({ vpnPolicy: 'force-physical' });
 *     const conn = ws.createConnection();
 *     conn.on('data', (buf) => handle(buf));
 *     await conn.connect('wss://sfu.example.com/signal', 5000);
 **/
ttsignal.createWsConnector = function (config) {
    ttRequireStack('__createWsConnector__', 'WebSocket 栈');
    return ttsignal.__createWsConnector__(
        ttNormalizeStackConfig(config, 'createWsConnector'));
};


//*******************************************************************************
// Exports :
//
// 历史形态是 `exports.ttsignal = ttsignal`，于是 `require('ttsignal')` 拿到的是一层
// 包装对象 —— `require('ttsignal').createConnector` 是 undefined。仓库里 js/client.js
// / js/server.js / js/getLogFile.js / js/upload-livekit-bin.js 等示例脚本正是那么写
// 的，一直跑不起来（既有问题，不是本次引入的）。这里把**模块本身**导出，并保留一个
// 不可枚举的 `.ttsignal` 自引用，两种写法都成立：
//
//   const tts = require('ttsignal');            // tts.createConnector / createHttpConnector
//   const { ttsignal } = require('ttsignal');   // 旧写法（js/pathChange.js），仍然可用
//
// 这是一次**放宽**，不是替换：旧调用方读到的 `.ttsignal` 还是同一个对象。
// rtc-client 的 src/signaling/ttsignal-module.ts 做的正是 `wrapped.ttsignal ?? wrapped`，
// 两种形态都吃，所以对它没有影响。自引用设为不可枚举，避免 console.dir / 遍历导出对象
// 时多出一层环。
//*******************************************************************************

module.exports = ttsignal;
Object.defineProperty(module.exports, 'ttsignal', {
    value: ttsignal, enumerable: false, writable: true, configurable: true
});

//*******************************************************************************
// End of file : ttsignal.js
//*******************************************************************************
