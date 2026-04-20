/*******************************************************************************
 * javascript glue wrapper of ttsignal module
 * @module index.js
 * @author anto.
*******************************************************************************/

var util			= require('util');
var EventEmitter    = require('events').EventEmitter;

var ttsignal             = null;

if (process.ttsBuildType == 'debug') {
    ttsignal = require('./Debug/ttsignal.'+process.platform+'.'+process.arch+'.node');
} else {
    ttsignal = require('./Release/ttsignal.'+process.platform+'.'+process.arch+'.node');
}

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
 * @example
 *     var succeed = ttsignal.createConnector(config);
 **/
ttsignal.createConnector = function(config){
    if (typeof config !== 'object') {
        throw Error('Invalid config.');
    }
    if (!config.alpn){
        throw Error('Invalid alpn value.');
    }
    if (config.caCertPem && !config.ca_cert_pem) {
        config.ca_cert_pem = config.caCertPem;
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
// Exports : 
//*******************************************************************************

module.exports = ttsignal;

//*******************************************************************************
// End of file : ttsignal.js
//*******************************************************************************
