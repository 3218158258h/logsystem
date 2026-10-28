#!/usr/bin/env python
# coding=utf-8
import fdbus
import sys
import os
import time
import json

create_all_data = 0
subscribe_all_data = 0

class myDataChangeClosure(fdbus.DataChangeClosure):
    def __init__(self, dp):
        self.data_pool = dp

    def handleDataChange(self, topic_id, topic_str, msg_data):
        print('client modify data: domain: %d, topic id: %d, topic name: %s, data: %s'%(self.data_pool.getDomainId(), topic_id, topic_str, fdbus.castToPChar(msg_data)))
        data = 'owner update and publish'
        self.data_pool.publishData(topic_id, topic_str, fdbus.castToCChar(data))

class myDataNotifyClosure(fdbus.DataNotifyClosure):
    def __init__(self, dp):
        self.data_pool = dp

    def handleDataNotify(self, topic_id, topic_str, msg_data):
        print('server publish data: domain: %d, topic id: %d, topic name: %s, data: %s'%(self.data_pool.getDomainId(), topic_id, topic_str, fdbus.castToPChar(msg_data)))

fdb_domains = [0, 1, 2]
fdb_topic_set_a = ['topic1', 'topic2', "topic3", "topic4"]
fdb_topic_set_b = ["topic11", "topic12", "topic13", "topic14"]
fdb_topic_set_ids = [0, 1, 2, 3]

if len(sys.argv) < 2:
    print("help:\nfdbtestclibdatapool 0|1\n 0: mode 0; 1: mode 1")
    sys.exit(-1)

fdb_test_mode = sys.argv[1]
fdbus.fdbusStart(os.getenv('FDB_CLIB_PATH'))

dp_list = []
for i in fdb_domains:
    data_pool = fdbus.FdbusDataPool(i, 'python-data-pool-' + str(i))
    data_pool.start(True, True)
    dp_list.append(data_pool)

init_data = 'initial topic value'
for dp in dp_list:
    if fdb_test_mode == '0':
        own_topic_set = fdb_topic_set_a
    else:
        own_topic_set = fdb_topic_set_b
    if create_all_data:
        dp.createAllData(myDataChangeClosure(dp))
    else:
        i = 0
        for id in fdb_topic_set_ids:
            dp.createData(id, own_topic_set[i], myDataChangeClosure(dp), init_data)
            i += 1

for dp in dp_list:
    if fdb_test_mode == '0':
        borrow_topic_set = fdb_topic_set_b
    else:
        borrow_topic_set = fdb_topic_set_a
    if subscribe_all_data:
        dp.subscribeAllData( myDataNotifyClosure(dp))
    else:
        i = 0
        for id in fdb_topic_set_ids:
            dp.subscribeData(id, borrow_topic_set[i], myDataNotifyClosure(dp))
            i += 1

publish_data = 'published data'
data = {'key1' : 'value1', 'key2' : 'value2'}
while True:
    for dp in dp_list:
        if fdb_test_mode == '0':
            borrow_topic_set = fdb_topic_set_b
        else:
            borrow_topic_set = fdb_topic_set_a
        i = 0
        for id in fdb_topic_set_ids:
            dp.publishData(id, borrow_topic_set[i], fdbus.castToCChar(json.dumps(data)))
            i += 1
        time.sleep(0.1)

