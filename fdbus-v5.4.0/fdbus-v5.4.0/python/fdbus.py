"""
/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
"""

import sys
import ctypes
import os
import threading

fdb_clib = None

FDB_QOS_LOCAL = 0
FDB_QOS_RELIABLE = 1
FDB_QOS_BEST_EFFORTS = 2
FDB_QOS_SECURE_RELIABLE = 3
FDB_QOS_SECURE_BEST_EFFORTS = 4
FDB_QOS_TRY_SECURE_RELIABLE = 5
FDB_QOS_TRY_SECURE_BEST_EFFORTS = 6
FDB_QOS_INVALID = 7

def castToCChar(wchar):
    if wchar:
        return wchar.encode('utf-8')
    else:
        return wchar

def castToPChar(wchar):
    if wchar:
        return wchar.decode('utf-8')
    else:
        return wchar

def fdbLogTrace(level, tag, *argv):
    global fdb_clib
    fdb_clib.fdb_log_trace.argtypes = [ctypes.c_int,        #log_level
                                       ctypes.c_char_p,     #tag
                                       ctypes.c_char_p      #data
                                      ]
    log_data = ''
    for i in argv:
        log_data += str(i)
    
    fdb_clib.fdb_log_trace(level, castToCChar(tag), castToCChar(log_data))

def FDB_LOG_D(tag, *argv):
    fdbLogTrace(1, tag, argv)

def FDB_LOG_I(tag, *argv):
    fdbLogTrace(2, tag, argv)

def FDB_LOG_W(tag, *argv):
    fdbLogTrace(3, tag, argv)

def FDB_LOG_E(tag, *argv):
    fdbLogTrace(4, tag, *argv)

def FDB_LOG_F(tag, *argv):
    fdbLogTrace(5, tag, argv)

# private function
def fdbusCtypes2buffer(cptr, length):
    """Convert ctypes pointer to buffer type.

    Parameters
    ----------
    cptr : ctypes.POINTER(ctypes.c_byte)
        Pointer to the raw memory region.
    length : int
        The length of the buffer.

    Returns
    -------
    buffer : bytearray
        The raw byte memory buffer.
    """
    #if not isinstance(cptr, ctypes.POINTER(ctypes.c_char)):
    #    raise TypeError('expected char pointer')
    if not bool(cptr) or not length:
        return None

    res = bytearray(length)
    rptr = (ctypes.c_byte * length).from_buffer(res)
    if not ctypes.memmove(rptr, cptr, length):
        return None
    return bytes(res)

class ReplyClosure(object):
    def handleReply(self, sid, msg_code, msg_data, status):
        pass
    def setClient(self, client):
        self.client = client
    def getReplyCallback(self):
        def _handleReply(handle, msg):
            try:
                self.handleReply(msg.contents.sid,
                                 msg.contents.msg_code,
                                 fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                                 msg.contents.status)
            except Exception as e:
                print('Except in handleReply: ', e)
            if (self.client):
                self.client.removeReply(self)

        msg_handle = fdb_message_reply_fn_t(_handleReply)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(msg_handle)
        return msg_handle

class SubscribeItem(ctypes.Structure):
            _fields_ = [('event_code', ctypes.c_int), ('topic', ctypes.c_char_p)]

class ReturnMessage(ctypes.Structure):
    _fields_ = [('sid', ctypes.c_int),
                ('msg_code', ctypes.c_int),
                ('msg_data', ctypes.POINTER(ctypes.c_byte)), 
                ('data_size', ctypes.c_int),
                ('status', ctypes.c_int),
                ('topic', ctypes.c_char_p),
                ('user_data', ctypes.c_void_p),
                ('qos', ctypes.c_int),
                ('_msg_buffer', ctypes.c_void_p),]

fdb_client_online_fn_t = ctypes.CFUNCTYPE(None,                                  #return
                                          ctypes.c_void_p,                       #handle
                                          ctypes.c_int,                          #sid
                                          ctypes.c_int                           #qos
                                          )
fdb_client_offline_fn_t = ctypes.CFUNCTYPE(None,                                 #return
                                           ctypes.c_void_p,                      #handle
                                           ctypes.c_int,                         #sid
                                           ctypes.c_int                          #qos
                                           )
fdb_client_reply_fn_t = ctypes.CFUNCTYPE(None,                                   #return
                                         ctypes.c_void_p,                        #handle
                                         ctypes.POINTER(ReturnMessage)           #message
                                         )
fdb_message_reply_fn_t = ctypes.CFUNCTYPE(None,                                  #return
                                          ctypes.c_void_p,                       #handle
                                          ctypes.POINTER(ReturnMessage)          #message
                                          )
fdb_client_get_event_fn_t = ctypes.CFUNCTYPE(None,                               #return
                                             ctypes.c_void_p,                    #handle
                                             ctypes.POINTER(ReturnMessage)       #message
                                         )
fdb_client_broadcast_fn_t = ctypes.CFUNCTYPE(None,                               #return
                                             ctypes.c_void_p,                    #handle
                                             ctypes.POINTER(ReturnMessage)       #message
                                             )

class ClientHandles(ctypes.Structure):
    _fields_ = [('on_online', fdb_client_online_fn_t),
                ('on_offline', fdb_client_offline_fn_t),
                ('on_reply', fdb_client_reply_fn_t),
                ('on_get_event', fdb_client_get_event_fn_t),
                ('on_broadcast', fdb_client_broadcast_fn_t)]

# public function
# initialize FDBus; should be called before any call to FDBus
def fdbusStart(clib_path = None):
    global fdb_clib
    os_is = sys.platform.startswith
    if os_is("win32"):
        dll = "fdbus-clib.dll"
    else:
        dll = "libfdbus-clib.so"

    if clib_path:
        dll = os.path.join(clib_path, dll)
    fdb_clib = ctypes.CDLL(dll)
    fdb_clib.fdb_start()

# base class of FDBus client
class FdbusClient(object):
    # create FDBus client.
    # @name - name of client endpoint for debug purpose
    def __init__(self, name, native_handle = None):
        global fdb_clib
        if fdb_clib is None:
            e = ValueError()
            e.strerror = 'fdbus is not started! Did fdbusStart() called?'
            raise(e)

        self.name = name
        self.handles = ClientHandles()
        self.handles.on_online = self.getOnOnlineFunc()
        self.handles.on_offline = self.getOnOfflineFunc()
        self.handles.on_reply = self.getOnReplyFunc()
        self.handles.on_get_event = self.getOnGetEventFunc()
        self.handles.on_broadcast = self.getOnBroadcast()
        if native_handle is None:
            fn_create = fdb_clib.fdb_client_create
            fn_create.restype = ctypes.c_void_p
            self.native = fn_create(name, None)
            fdb_clib.fdb_client_register_event_handle.argtypes = [ctypes.c_void_p, ctypes.POINTER(ClientHandles)]
            fdb_clib.fdb_client_register_event_handle(self.native, ctypes.byref(self.handles))
        else:
            self.native = native_handle

        self.mutex = threading.Lock()
        self.pending_reply = {}

    # private method
    def getOnOnlineFunc(self):
        def callOnOnline(handle, sid, qos):
            try:
                self.onOnline(sid, qos)
            except Exception as e:
                print('Except in onOnline: ', e)
        return fdb_client_online_fn_t(callOnOnline)

    # private method
    def getOnOfflineFunc(self):
        def callOnOffline(handle, sid, qos):
            try:
                self.onOffline(sid, qos)
            except Exception as e:
                print('Except in onOffline: ', e)
        return fdb_client_offline_fn_t(callOnOffline)

    # private method
    def getOnReplyFunc(self):
        def callOnReply(handle, msg):
            try:
                self.onReply(msg.contents.sid,
                             msg.contents.msg_code,
                             fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                             msg.contents.status,
                             msg.contents.user_data)
            except Exception as e:
                print('Except in onReply: ', e)
        return fdb_client_reply_fn_t(callOnReply)

    # private method
    def getOnGetEventFunc(self):
        def callOnGetEvent(handle, msg):
            try:
                self.onGetEvent(msg.contents.sid,
                                msg.contents.msg_code,
                                castToPChar(msg.contents.topic),
                                fdbusCtypes2buffer(msg.contents.event_data, msg.contents.data_size),
                                msg.contents.status,
                                msg.contents.user_data)
            except Exception as e:
                print('Except in onGetEvent: ', e)
        return fdb_client_get_event_fn_t(callOnGetEvent)

    # private method
    def getOnBroadcast(self):
        def callOnBroadcast(handle, msg):
            try:
                self.onBroadcast(msg.contents.sid,
                                 msg.contents.msg_code,
                                 fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                                 castToPChar(msg.contents.topic))
            except Exception as e:
                print('Except in onBroadcast: ', e)
        return fdb_client_broadcast_fn_t(callOnBroadcast)

    # public method
    # connect to server
    # @url - address of the server in format 'svc://server_name
    def connect(self, url, timeout = -1):
        global fdb_clib
        fdb_clib.fdb_client_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        fdb_clib.fdb_client_connect(self.native, castToCChar(url), timeout)

    # public method
    # disconnect with server
    def disconnect(self):
        global fdb_clib
        fdb_clib.fdb_client_disconnect.argtypes = [ctypes.c_void_p]
        fdb_clib.fdb_client_disconnect(self.native)

    """
    public method
    invoke server method asynchronously: will return immediately and
        the reply will be received from onReply()
    @msg_code(int) - message code
    @msg_data(str) - data of the message
    @timeout(int) - timer of the call; if not reply before timeout,
        status will received
    @user_data(int) - user data provided by user and will be
        returned at onReply()
    @log_data(str) - extra information that will be printed in logsvc
    """
    def invoke_async(self,
                     msg_code,
                     msg_data = None,
                     timeout = 0,
                     qos = FDB_QOS_TRY_SECURE_RELIABLE,
                     user_data = None,
                     log_data = None):
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        global fdb_clib
        fdb_clib.fdb_client_invoke_async.argtypes = [ctypes.c_void_p,
                                                     ctypes.c_int,
                                                     ctypes.c_char_p,
                                                     ctypes.c_int,
                                                     ctypes.c_int,
                                                     ctypes.c_int,
                                                     ctypes.c_void_p,
                                                     ctypes.c_char_p]
        fdb_clib.fdb_client_invoke_async(self.native,
                                         msg_code,
                                         msg_data,
                                         data_size,
                                         timeout,
                                         qos,
                                         user_data,
                                         castToCChar(log_data))

    def invoke_callback(self,
                        callback,
                        msg_code,
                        msg_data = None,
                        timeout = 0,
                        qos = FDB_QOS_TRY_SECURE_RELIABLE,
                        log_data = None):
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        global fdb_clib
        fdb_clib.fdb_client_invoke_callback.argtypes = [ctypes.c_void_p,
                                                        ctypes.c_int,
                                                        ctypes.c_char_p,
                                                        ctypes.c_int,
                                                        ctypes.c_int,
                                                        ctypes.c_int,
                                                        fdb_message_reply_fn_t,
                                                        ctypes.c_void_p,
                                                        ctypes.c_char_p]
        fdb_clib.fdb_client_invoke_callback(self.native,
                                            msg_code,
                                            msg_data,
                                            data_size,
                                            timeout,
                                            qos,
                                            callback.getReplyCallback(),
                                            None,
                                            castToCChar(log_data))
        callback.setClient(self)
        self.mutex.acquire()
        self.pending_reply[callback] = callback
        self.mutex.release()

    def removeReply(self, reply):
        self.mutex.acquire()
        try:
            self.pending_reply.pop(reply)
        except Exception:
            pass
        self.mutex.release()

    """
    public method
    invoke server method synchronously: will block until reply is received.
    @msg_code(int) - message code
    @msg_data(str) - data of the message
    @timeout(int) - timer of the call; if not reply before timeout,
        status will received
    @log_data(str) - extra information that will be printed in logsvc
    @return - data returned in dictionary:
        'sid'(int) - session id
        'msg_code'(int) - message code
        'msg_data'(str) - message data
        'status'(int) - return status
    """
    def invoke_sync(self,
                    msg_code,
                    msg_data = None,
                    timeout = 0,
                    qos = FDB_QOS_TRY_SECURE_RELIABLE,
                    log_data = None):
        global fdb_clib
        ret = ReturnMessage()
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        fdb_clib.fdb_client_invoke_sync.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_int,
                                                    ctypes.c_char_p,
                                                    ctypes.c_int,
                                                    ctypes.c_int,
                                                    ctypes.c_int,
                                                    ctypes.c_char_p,
                                                    ctypes.POINTER(ReturnMessage)]
        fdb_clib.fdb_client_invoke_sync(self.native,
                                        msg_code,
                                        msg_data,
                                        data_size,
                                        timeout,
                                        qos,
                                        castToCChar(log_data),
                                        ctypes.byref(ret))
        return {'sid' : ret.sid,
                'msg_code' : ret.msg_code,
                'msg_data' : fdbusCtypes2buffer(ret.msg_data, ret.data_size),
                'status' : ret.status,
                'qos' : ret.qos,
                '_msg_buffer' : ret._msg_buffer}

    """
    public method
    invoke server method asynchronously; no reply is expected.
    @msg_code(int) - message code
    @msg_data(str) - data of the message
    @log_data(str) - extra information that will be printed in logsvc
    """
    def send(self,
             msg_code,
             msg_data = None,
             qos = FDB_QOS_TRY_SECURE_RELIABLE,
             log_data = None):
        global fdb_clib
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        fdb_clib.fdb_client_send.argtypes = [ctypes.c_void_p,
                                             ctypes.c_int,
                                             ctypes.c_char_p,
                                             ctypes.c_int,
                                             ctypes.c_int,
                                             ctypes.c_char_p]
        fdb_clib.fdb_client_send(self.native,
                                 msg_code,
                                 msg_data,
                                 data_size,
                                 qos,
                                 castToCChar(log_data))

    def publish(self,
                event,
                topic = None,
                event_data = None,
                qos = FDB_QOS_TRY_SECURE_RELIABLE,
                log_data = None,
                always_update = False):
        global fdb_clib
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)

        fdb_clib.fdb_client_publish.argtypes = [ctypes.c_void_p,
                                                ctypes.c_int,
                                                ctypes.c_char_p,
                                                ctypes.c_char_p,
                                                ctypes.c_int,
                                                ctypes.c_int,
                                                ctypes.c_char_p,
                                                ctypes.c_bool]
        fdb_clib.fdb_client_publish(self.native,
                                    event,
                                    castToCChar(topic),
                                    event_data,
                                    data_size,
                                    qos,
                                    castToCChar(log_data),
                                    always_update)

    def get_async(self,
                  event,
                  topic = None,
                  timeout = 0,
                  qos = FDB_QOS_TRY_SECURE_RELIABLE,
                  user_data = None):
        global fdb_clib
        fdb_clib.fdb_client_get_event_async.argtypes = [ctypes.c_void_p,
                                                        ctypes.c_int,
                                                        ctypes.c_char_p,
                                                        ctypes.c_int,
                                                        ctypes.c_int,
                                                        ctypes.c_void_p
                                                        ]
        fdb_clib.fdb_client_get_event_async(self.native,
                                            event,
                                            castToCChar(topic),
                                            timeout,
                                            qos,
                                            user_data)

    def get_sync(self,
                 event,
                 topic = None,
                 timeout = 0,
                 qos = FDB_QOS_TRY_SECURE_RELIABLE):
        global fdb_clib
        ret = ReturnMessage()
        fdb_clib.fdb_client_get_event_sync.argtypes = [ctypes.c_void_p,
                                                       ctypes.c_int,
                                                       ctypes.c_char_p,
                                                       ctypes.c_int,
                                                       ctypes.c_int,
                                                       ctypes.POINTER(ReturnMessage)
                                                       ]
        fdb_clib.fdb_client_get_event_sync(self.native,
                                           event,
                                           castToCChar(topic),
                                           timeout,
                                           qos,
                                           ctypes.byref(ret))
        return {'sid' : ret.sid,
                'event' : ret.msg_code,
                'topic' : topic,
                'event_data' : fdbusCtypes2buffer(ret.msg_data, ret.data_size),
                'status' : ret.status,
                'qos' : ret.qos,
                '_msg_buffer' : ret._msg_buffer}

    """
    public method
    subscribe list of events upon the server
    event_list(array of dict): list of events to subscribe.
        event_list[n]['event_code'](int) - event code or
        event_list[n]['group'](int) - event group
        event_list[n]['topic'](str) - topic
    """
    def subscribe(self, event_list, qos = FDB_QOS_TRY_SECURE_RELIABLE):
        global fdb_clib
        class SubscribeItem(ctypes.Structure):
            _fields_ = [('event_code', ctypes.c_int), ('topic', ctypes.c_char_p)]

        subscribe_items = (SubscribeItem * len(event_list))()
        for i in range(len(event_list)):
            code = event_list[i].get('event_code', None)
            if not code:
                code = event_list[i].get('group', None)
                if code:
                    code = ((code & 0xff) << 24) | 0xffffff
            if code:
                subscribe_items[i].event_code = ctypes.c_int(code)
                subscribe_items[i].topic = ctypes.c_char_p(castToCChar(event_list[i]['topic']))

        # what if topic is None?
        fdb_clib.fdb_client_subscribe.argtypes = [ctypes.c_void_p,
                                                  ctypes.POINTER(SubscribeItem),
                                                  ctypes.c_int,
                                                  ctypes.c_int]
        fdb_clib.fdb_client_subscribe(self.native, subscribe_items, len(subscribe_items), qos)
        
    """
    public method
    unsubscribe list of events upon the server
    event_list - the same as subscribe()
    """
    def unsubscribe(self, event_list, qos = FDB_QOS_TRY_SECURE_RELIABLE):
        global fdb_clib
        subscribe_items = (SubscribeItem * len(event_list))()
        for i in range(len(event_list)):
            subscribe_items[i].event_code = ctypes.c_int(event_list[i]['event_code'])
            subscribe_items[i].topic = ctypes.c_char_p(castToCChar(event_list[i]['topic']))

        fdb_clib.fdb_client_unsubscribe.argtypes = [ctypes.c_void_p,
                                                    ctypes.POINTER(SubscribeItem),
                                                    ctypes.c_int,
                                                    ctypes.c_int]
        fdb_clib.fdb_client_unsubscribe(self.native, subscribe_items, len(subscribe_items), qos)
    
    """
    Callback method and should be overrided
    will be called when the client is connected with server
    @sid - session id
    """
    def onOnline(self, sid, qos):
        print('onOnline for client ', self.name, ', qos: ', qos)
    
    """
    Callback method
    will be called when the client is disconnected with server
    @sid(int) - session id
    """
    def onOffline(self, sid, qos):
        print('onOffline for client ', self.name, ', qos: ', qos)
    
    """
    Callback method and should be overrided
    will be called when (asynchronous) method call is replied
    @sid(int) - session id
    @msg_code(int) - message code
    @msg_data(str) - message data
    @status(int) - status replied from server
    @user_data(int) - data given by invoke_async() and return to the user here
    """
    def onReply(self, sid, msg_code, msg_data, status, user_data):
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        print('onReply for client ', self.name,
              ', code: ', msg_code,
              ', size: ', data_size)

    def onGetEvent(self, sid, event, topic, event_data, status, user_data):
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)
        print('onGetEvent for client ', self.name,
              ', code: ', event,
              ', topic: ', topic,
              ', size: ', data_size)
        
    """
    Callback method and should be overrided
    will be called when events are broadcasted from server
    @sid(int) - session id
    @event_code(int) - event code
    @event_data(str) - event data
    @topic(str) - topic of the event
    """
    def onBroadcast(self, sid, event_code, event_data, topic):
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)
        print('onBroadcast for client ', self.name,
              ', code: ', event_code,
              ', topic: ', topic,
              ', size: ', data_size)

def releaseReturnMsg(ret_msg):
    global fdb_clib
    c_ret_msg = ReturnMessage()
    c_ret_msg._msg_buffer = ret_msg['_msg_buffer']
    fdb_clib.fdb_client_release_return_msg.argtypes = [ctypes.POINTER(ReturnMessage)]
    fdb_clib.fdb_client_release_return_msg(ctypes.byref(c_ret_msg))


# class acting as handle to reply message to the calling client
class FdbusReplyHandle():
    def __init__(self, reply_handle):
        self.reply_handle = reply_handle
    
    """
    public method
    reply message to the calling client
    @msg_data(str) - message data
    @log_data(str) - extra information that will be printed in logsvc
    note that message code is the same as that of method call
    """
    def reply(self, msg_data = None, log_data = None):
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        global fdb_clib
        fdb_clib.fdb_message_reply.argtypes = [ctypes.c_void_p,
                                               ctypes.c_char_p,
                                               ctypes.c_int,
                                               ctypes.c_char_p]
        fdb_clib.fdb_message_reply(self.reply_handle,
                                   msg_data,
                                   data_size,
                                   castToCChar(log_data))
        self.reply_handle = None
    
    """
    public method
    broadcast message to the client subscribing the event
    @event_code(int) - event code
    @event_data(str) - event data
    @topic(str) - topic of the event
    @log_data(str) - extra information that will be printed in logsvc
    """
    def broadcast(self, event_code, event_data = None, topic = None, log_data = None):
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)
        global fdb_clib
        fdb_clib.fdb_message_broadcast.argtypes = [ctypes.c_void_p,
                                                   ctypes.c_int,
                                                   ctypes.c_char_p,
                                                   ctypes.c_char_p,
                                                   ctypes.c_int,
                                                   ctypes.c_void_p]
        fdb_clib.fdb_message_broadcast(self.reply_handle,
                                       event_code,
                                       castToCChar(topic),
                                       event_data,
                                       data_size,
                                       log_data)
        
    """
    public method
    release resources occupied by the handle. should be called after method
    call is replied (with reply() or event is broadcasted (with broadcast()
    or memory leakage happens
    """
    def destroy(self):
        global fdb_clib
        if self.reply_handle is None:
            return;
        fdb_clib.fdb_message_destroy.argtypes = [ctypes.c_void_p]
        fdb_clib.fdb_message_destroy(self.reply_handle)
        self.reply_handle = None
    
fdb_server_online_fn_t = ctypes.CFUNCTYPE(None,                             #return
                                          ctypes.c_void_p,                  #handle
                                          ctypes.c_int,                     #sid
                                          ctypes.c_byte,                    #is_first
                                          ctypes.c_int                       #qos
                                          )
fdb_server_offline_fn_t = ctypes.CFUNCTYPE(None,                            #return
                                           ctypes.c_void_p,                 #handle
                                           ctypes.c_int,                    #sid
                                           ctypes.c_byte,                   #is_last
                                           ctypes.c_int                     #qos
                                           )
fdb_server_invoke_fn_t = ctypes.CFUNCTYPE(None,                             #return
                                          ctypes.c_void_p,                  #handle
                                          ctypes.POINTER(ReturnMessage),    #message
                                          ctypes.c_void_p                   #reply_handle
                                          )
fdb_server_subscribe_fn_t = ctypes.CFUNCTYPE(None,                          #return
                                             ctypes.c_void_p,               #handle
                                             ctypes.POINTER(SubscribeItem), #subscribe_items
                                             ctypes.c_int,                  #nr_items
                                             ctypes.c_void_p                #reply_handle
                                             )

class ServerHandles(ctypes.Structure):
    _fields_ = [('on_online', fdb_server_online_fn_t),
                ('on_offline', fdb_server_offline_fn_t),
                ('on_invoke', fdb_server_invoke_fn_t),
                ('on_subscribe', fdb_server_subscribe_fn_t)]

#base class of FDBus Server
class FdbusServer(object):
    # create FDBus server
    # name: name of the server for debug purpose
    def __init__(self, name, native_handle = None):
        global fdb_clib
        if fdb_clib is None:
            e = ValueError()
            e.strerror = 'fdbus is not started! Did fdbusStart() called?'
            raise(e)
        self.name = name

        self.handles = ServerHandles()
        self.handles.on_online = self.getOnOnlineFunc()
        self.handles.on_offline = self.getOnOfflineFunc()
        self.handles.on_invoke = self.getOnInvokeFunc()
        self.handles.on_subscribe = self.getOnSubscribeFunc()

        if native_handle is None:
            fn_create = fdb_clib.fdb_server_create
            fn_create.restype = ctypes.c_void_p
            self.native = fn_create(name, None)
            fdb_clib.fdb_server_register_event_handle.argtypes = [ctypes.c_void_p, ctypes.POINTER(ServerHandles)]
            fdb_clib.fdb_server_register_event_handle(self.native, ctypes.byref(self.handles))
        else:
            self.native = native_handle

    # private method
    def getOnOnlineFunc(self):
        def callOnOnline(handle, sid, is_first, qos):
            try:
                self.onOnline(sid, bool(is_first), qos)
            except Exception as e:
                print('Except in onOnline: ', e)
        return fdb_server_online_fn_t(callOnOnline)

    # private method
    def getOnOfflineFunc(self):
        def callOnOffline(handle, sid, is_last, qos):
            try:
                self.onOffline(sid, bool(is_last), qos)
            except Exception as e:
                print('Except in onOffline: ', e)
        return fdb_server_offline_fn_t(callOnOffline)

    # private method
    def getOnInvokeFunc(self):
        def callOnInvoke(handle, msg, reply_handle):
            try:
                self.onInvoke(msg.contents.sid,
                              msg.contents.msg_code,
                              fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                              FdbusReplyHandle(reply_handle))
            except Exception as e:
                print('Except in onInvoke: ', e)
        return fdb_server_invoke_fn_t(callOnInvoke)

    # private method
    def getOnSubscribeFunc(self):
        def callOnSubscribe(handle, subscribe_items, nr_items, reply_handle):
            p = ctypes.cast(subscribe_items, ctypes.POINTER(SubscribeItem * nr_items))
            c_items = p.contents
            items = []
            for i in range(nr_items):
                items.append({'event_code': c_items[i].event_code,
                              'topic' : str(c_items[i].topic)})

            handle = FdbusReplyHandle(reply_handle)
            try:
                self.onSubscribe(items, handle)
            except Exception as e:
                print('Except in onSubscribe: ', e)
            handle.destroy();
        return fdb_server_subscribe_fn_t(callOnSubscribe)

    # public method
    # bind server with a address
    # @url(str) - address of the server in format 'svc://server_name
    def bind(self, url):
        global fdb_clib
        fdb_clib.fdb_server_bind.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        fdb_clib.fdb_server_bind(self.native, castToCChar(url))
        
    # public method
    # unbind server with a address
    def unbind(self):
        global fdb_clib
        fdb_clib.fdb_server_unbind.argtypes = [ctypes.c_void_p]
        fdb_clib.fdb_server_unbind(self.native)
    
    """
    public method
    broadcast event to the clients subscribed the event
    @event_code(int) - code of the event
    @event_data(str) - data of the event
    @topic(str) - topic of the event
    @log_data(str) - extra information that will be printed in logsvc
    """
    def broadcast(self,
                  event_code,
                  event_data = None,
                  topic = None,
                  qos = FDB_QOS_TRY_SECURE_RELIABLE,
                  log_data = None):
        global fdb_clib
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)
        fdb_clib.fdb_server_broadcast.argtypes = [ctypes.c_void_p,
                                                   ctypes.c_int,
                                                   ctypes.c_char_p,
                                                   ctypes.c_char_p,
                                                   ctypes.c_int,
                                                   ctypes.c_void_p]
        fdb_clib.fdb_server_broadcast(self.native,
                                       event_code,
                                       castToCChar(topic),
                                       event_data,
                                       data_size,
                                       qos,
                                       log_data)

    def enable_event_cache(self, enable):
        global fdb_clib
        fdb_clib.fdb_server_enable_event_cache.argtypes = [ctypes.c_void_p,
                                                           ctypes.c_bool]
        fdb_clib.fdb_server_enable_event_cache(self.native, enable)

    def init_event_cache(self, event, topic, event_data, allow_event_route):
        global fdb_clib
        if event_data is None:
            data_size = 0
        else:
            data_size = len(event_data)

        fdb_clib.fdb_server_init_event_cache.argtypes = [ctypes.c_void_p,
                                                         ctypes.c_int,
                                                         ctypes.c_char_p,
                                                         ctypes.c_char_p,
                                                         ctypes.c_int,
                                                         ctypes.c_bool]
        fdb_clib.fdb_server_init_event_cache(self.native,
                                             event,
                                             castToCChar(topic),
                                             event_data,
                                             data_size,
                                             allow_event_route)

    """
    Callback method and should be overrided
    called when a connect is connected
    @sid(int) - session ID
    @is_first(bool) - True if this is the first connected client; otherwise False
    """
    def onOnline(self, sid, is_first, qos):
        print('onOnline for server ', self.name, ', first: ', is_first, ', qos: ', qos)
    
    """
    Callback method and should be overrided
    called when a client is connected
    @sid(int) - session ID
    @is_last(bool) - True if this is the last connected client; otherwise False
    """
    def onOffline(self, sid, is_last, qos):
        print('onOffline for server ', self.name, ', last: ', is_last, ', qos', qos)

    """
    Callback method and should be overrided
    called when methods are called from clients
    @sid(int) - session ID
    @msg_code(int) - code of message
    @msg_data(str) - data of message
    @reply_handle(FdbusReplyHandle) - handle used to reply message to the
        calling client
    """
    def onInvoke(self, sid, msg_code, msg_data, reply_handle):
        if msg_data is None:
            data_size = 0
        else:
            data_size = len(msg_data)
        print('onInvoke for server ', self.name,
              ', code: ', msg_code,
              ', size: ', data_size)

    """
    Callback method and should be overrided
    called when clients are subscribing events
    @event_list(int) - list of events subscribed. refer to FdbusClient::subscribe()
    @reply_handle(FdbusReplyHandle) - handle used to reply initial value of events
        to calling client
    """
    def onSubscribe(self, event_list, reply_handle):
        print('onSubscribe for server ', self.name)
        for i in range(len(event_list)):
            print('    event: ', event_list[i]['event_code'],
                  ', topic: ', event_list[i]['topic'])

fdb_comp_connection_fn_t = ctypes.CFUNCTYPE(None,                               #return
                                            ctypes.c_int,                       #sid
                                            ctypes.c_byte,                      #is_online
                                            ctypes.c_byte,                      #first_or_last
                                            ctypes.c_void_p,                    #user_data
                                            ctypes.c_int,                       #qos
                                          )
fdb_comp_event_handle_fn_t = ctypes.CFUNCTYPE(None,                             #return
                                              ctypes.POINTER(ReturnMessage)     #message
                                              )
fdb_comp_message_handle_fn_t = ctypes.CFUNCTYPE(None,                           #return
                                                ctypes.POINTER(ReturnMessage),  #message
                                                ctypes.c_void_p                 #reply_handle
                                                )

class ConnectionClosure(object):
    def handleConnection(self, sid, is_online, first_or_last, qos):
        pass
    def getConnectionCallback(self):
        def _handleConnection(sid, is_online, first_or_last, user_data, qos):
            try:
                self.handleConnection(sid, is_online, first_or_last, qos);
            except Exception as e:
                print('Except in handleConnection: ', e)

        connection_handle = fdb_comp_connection_fn_t(_handleConnection)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(connection_handle)
        return connection_handle

class EventClosure(object):
    def handleEvent(self, sid, msg_code, msg_data, topic):
        pass
    def getEventCallback(self):
        def _handleEvent(msg):
            try:
                self.handleEvent(msg.contents.sid,
                                 msg.contents.msg_code,
                                 fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                                 msg.contents.topic);
            except Exception as e:
                print('Except in handleEvent: ', e)

        event_handle = fdb_comp_event_handle_fn_t(_handleEvent)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(event_handle)
        return event_handle

class MessageClosure(object):
    def handleMessage(self, sid, msg_code, msg_data, reply_handle):
        pass
    def getMessageCallback(self):
        def _handleMessage(msg, reply_handle):
            try:
                self.handleMessage(msg.contents.sid,
                                   msg.contents.msg_code,
                                   fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size),
                                   FdbusReplyHandle(reply_handle));
            except Exception as e:
                print('Except in handleMessage: ', e)

        message_handle = fdb_comp_message_handle_fn_t(_handleMessage)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(message_handle)
        return message_handle

class EventHandle(ctypes.Structure):
    _fields_ = [('evt_code', ctypes.c_int),
                ('topic', ctypes.c_char_p),
                ('fn', fdb_comp_event_handle_fn_t), 
                ('user_data', ctypes.c_void_p)]
class MessageHandle(ctypes.Structure):
    _fields_ = [('msg_code', ctypes.c_int),
                ('fn', fdb_comp_message_handle_fn_t), 
                ('user_data', ctypes.c_void_p)]
class FdbusAfComponent(object):
    def __init__(self, name):
        global fdb_clib
        if fdb_clib is None:
            e = ValueError()
            e.strerror = 'fdbus is not started! Did fdbusStart() called?'
            raise(e)
        self.name = name
        fn_create = fdb_clib.fdb_create_afcomponent
        fn_create.restype = ctypes.c_void_p
        self.native = fn_create(name)
        self.connection_callbacks = []
        self.event_handles = []
        self.message_handles = []

    def queryService(self, bus_name, event_handle_tbl, connection_callback):
        global fdb_clib
        event_tbl = None
        nr_handles = len(event_handle_tbl)
        if not event_handle_tbl is None:
            handle = (EventHandle * nr_handles)()
            for i in range(nr_handles):
                event_handle = event_handle_tbl[i]['callback']
                self.event_handles.append(event_handle)
                handle[i].evt_code = event_handle_tbl[i]['code']
                handle[i].topic = castToCChar(event_handle_tbl[i]['topic'])
                handle[i].fn = event_handle.getEventCallback()
                handle[i].user_data = None
            event_tbl = ctypes.cast(handle, ctypes.POINTER(EventHandle))

        if connection_callback:
            self.connection_callbacks.append(connection_callback)
            connection_callback_fn = connection_callback.getConnectionCallback()
        else:
            connection_callback_fn = None

        fn_query_service = fdb_clib.fdb_afcomponent_query_service
        fn_query_service.argtypes = [ctypes.c_void_p,
                                     ctypes.c_char_p,
                                     ctypes.POINTER(EventHandle),
                                     ctypes.c_int,
                                     fdb_comp_connection_fn_t]
        fn_query_service.restype = ctypes.c_void_p
        client_handle = fn_query_service(self.native,
                                         castToCChar(bus_name),
                                         event_tbl,
                                         nr_handles,
                                         connection_callback_fn)
        return FdbusClient(bus_name, client_handle)

    def offerService(self, bus_name, message_handle_tbl, connection_callback):
        global fdb_clib
        message_tbl = None
        nr_handles = len(message_handle_tbl)
        if not message_handle_tbl is None:
            handle = (MessageHandle * nr_handles)()
            for i in range(nr_handles):
                message_handle = message_handle_tbl[i]['callback']
                self.message_handles.append(message_handle)
                handle[i].msg_code = message_handle_tbl[i]['code']
                handle[i].fn = message_handle.getMessageCallback()
                handle[i].user_data = None
            message_tbl = ctypes.cast(handle, ctypes.POINTER(MessageHandle))

        if connection_callback:
            self.connection_callbacks.append(connection_callback)
            connection_callback_fn = connection_callback.getConnectionCallback()
        else:
            connection_callback_fn = None

        fn_offer_service = fdb_clib.fdb_afcomponent_offer_service
        fn_offer_service.argtypes = [ctypes.c_void_p,
                                     ctypes.c_char_p,
                                     ctypes.POINTER(MessageHandle),
                                     ctypes.c_int,
                                     fdb_comp_connection_fn_t]
        fn_offer_service.restype = ctypes.c_void_p
        server_handle = fn_offer_service(self.native,
                                         castToCChar(bus_name),
                                         message_tbl,
                                         nr_handles,
                                         connection_callback_fn)
        return FdbusServer(bus_name, server_handle)

fdb_data_change_fn_t = ctypes.CFUNCTYPE(None,                                  #return
                                        ctypes.c_void_p,                       #handle
                                        ctypes.POINTER(ReturnMessage)          #message
                                        )

class DataChangeClosure(object):
    def handleDataChange(self, topic_id, topic_str, msg_data):
        pass
    def getDataChangeCallback(self):
        def _handleDataChange(handle, msg):
            try:
                self.handleDataChange(msg.contents.msg_code,
                                      castToPChar(msg.contents.topic),
                                      fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size))
            except Exception as e:
                print('Except in _handleDataChange: ', e)

        data_change_handle = fdb_data_change_fn_t(_handleDataChange)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(data_change_handle)
        return data_change_handle

fdb_data_notify_fn_t = ctypes.CFUNCTYPE(None,                                  #return
                                        ctypes.c_void_p,                       #handle
                                        ctypes.POINTER(ReturnMessage)          #message
                                        )
class DataNotifyClosure(object):
    def handleDataNotify(self, topic_id, topic_str, msg_data):
        pass
    def getDataNotifyCallback(self):
        def _handleDataNotify(handle, msg):
            try:
                self.handleDataNotify(msg.contents.msg_code,
                                      castToPChar(msg.contents.topic),
                                      fdbusCtypes2buffer(msg.contents.msg_data, msg.contents.data_size))
            except Exception as e:
                print('Except in _handleDataNotify: ', e)

        data_notify_handle = fdb_data_notify_fn_t(_handleDataNotify)
        if not hasattr(self, 'handles'):
            self.handles = []
        self.handles.append(data_notify_handle)
        return data_notify_handle

# base class of FDBus data pool
class FdbusDataPool(object):
    # create FDBus data pool.
    # @name - name of client endpoint for debug purpose
    def __init__(self, domain_id, name):
        global fdb_clib
        if fdb_clib is None:
            e = ValueError()
            e.strerror = 'fdbus is not started! Did fdbusStart() called?'
            raise(e)

        self.name = name
        fn_create = fdb_clib.fdb_dp_create
        fn_create.restype = ctypes.c_void_p
        self.native = fn_create(domain_id, name, None)

        self.data_change_callbacks = []
        self.data_notify_callbacks = []

    def getDomainId(self):
        global fdb_clib
        fdb_clib.fdb_dp_get_domain.argtypes = [ctypes.c_void_p]
        return fdb_clib.fdb_dp_get_domain(self.native)

    def start(self, topic_owner, topic_borrower):
        global fdb_clib
        fdb_clib.fdb_dp_start.argtypes = [ctypes.c_void_p,
                                          ctypes.c_bool,
                                          ctypes.c_bool]
        fdb_clib.fdb_dp_start.restypes = ctypes.c_bool
        ret = fdb_clib.fdb_dp_start(self.native, topic_owner, topic_borrower)
        if ret:
            return True
        else:
            return False

    def createData(self, topic_id, topic_name, on_data_change_callback, init_data):
        if init_data is None:
            data_size = 0
        else:
            data_size = len(init_data)
        global fdb_clib
        self.data_change_callbacks.append(on_data_change_callback)
        fdb_clib.fdb_dp_create_data_ic.argtypes = [ctypes.c_void_p,
                                                   ctypes.c_int,
                                                   ctypes.c_char_p,
                                                   fdb_data_change_fn_t,
                                                   ctypes.c_void_p,
                                                   ctypes.c_int]
        fdb_clib.fdb_dp_create_data_ic(self.native,
                                            topic_id,
                                            castToCChar(topic_name),
                                            on_data_change_callback.getDataChangeCallback(),
                                            init_data,
                                            data_size)

    def publishData(self, topic_id, topic_name, data, force_update = True, qos = FDB_QOS_TRY_SECURE_RELIABLE):
        if data is None:
            data_size = 0
        else:
            data_size = len(data)

        global fdb_clib
        fdb_clib.fdb_dp_publish_data_ic.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_int,
                                                    ctypes.c_char_p,
                                                    ctypes.c_void_p,
                                                    ctypes.c_int,
                                                    ctypes.c_bool,
                                                    ctypes.c_int]
        fdb_clib.fdb_dp_publish_data_ic(self.native,
                                        topic_id,
                                        castToCChar(topic_name),
                                        data,
                                        data_size,
                                        force_update,
                                        qos)

    def subscribeData(self, topic_id, topic_name, on_data_notify_callback):
        global fdb_clib
        self.data_notify_callbacks.append(on_data_notify_callback)
        fdb_clib.fdb_dp_subscribe_data_ic.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_int,
                                                    ctypes.c_char_p,
                                                    fdb_data_notify_fn_t]
        fdb_clib.fdb_dp_subscribe_data_ic(self.native,
                                          topic_id,
                                          castToCChar(topic_name),
                                          on_data_notify_callback.getDataNotifyCallback())

    def createAllData(self, on_data_change_callback):
        global fdb_clib
        self.data_change_callbacks.append(on_data_change_callback)
        fdb_clib.fdb_dp_create_data.argtypes = [ctypes.c_void_p,
                                                   fdb_data_change_fn_t]
        fdb_clib.fdb_dp_create_data(self.native,
                                            on_data_change_callback.getDataChangeCallback())

    def subscribeAllData(self, on_data_notify_callback):
        global fdb_clib
        self.data_notify_callbacks.append(on_data_notify_callback)
        fdb_clib.fdb_dp_subscribe_data.argtypes = [ctypes.c_void_p,
                                                   fdb_data_notify_fn_t]
        fdb_clib.fdb_dp_subscribe_data(self.native,
                                          on_data_notify_callback.getDataNotifyCallback())

