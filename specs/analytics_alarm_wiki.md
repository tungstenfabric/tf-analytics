# Alarm debugging in Analytics

### 1. Introduction

------

In this document, explains about complete flow of analytics-alarm and what to collect when issues related to alarms is raised.

### 2. Problem Statement

------

There are cases when an alarm is not raised/cleared even when the condition is met/cleared.

### 3. Explanation

------

##### 3.1 Complete flow for raising an alarm when there is a change in UVE attribute

Alarmgen builds aggregated UVE on REDIS and raises an alarm when the alarm criteria is met. To balance the workload, alarmgen has 30 partitions. When each alarmgen starts, it connects to zookeeper to get partitions allocated. When collector receives UVE updates from different nodes, then collector writes the uve updates to local redis and it maps the UVE key to a partition and publishes notification onto Kafka for that specific partition. Kafka is the message bus cluster across all analytics nodes. Alarmgen for the specific partition will pick up the notification from kafka and process it.

After Alarmgen receives notification of UVE updates from Kafka, it reads and aggregates the UVE info from redis on all analytics nodes and publishes the aggregated UVE onto local redis.

Alarmgen have list of alarm rules which is matched then alarm will be raised when it satisfy the alarm condition. So, when alarmgen writes aggregated uve to local redis then it reads the UVE updates and matched it with alarm rules. If the condition satisfied then it raises the alarm.

##### 3.2  Verify messages in kafka topics for UVE change

Whenever there is an UVE update, Collector writes it on Kafka message bus so, we can verify those messages in Kafka to check whether or not collector writes it correctly to the Kafka. Following commands is used to check for messages on kafka bus:- Goto Kafka docker and execute

**List all consumer topics**: bin/kafka-topics.sh --zookeeper 10.204.216.10:2181 --describe

**List all consumer groups** bin/kafka-consumer-groups.sh --bootstrap-server 10.204.216.10:9092 --list

**Describe a consumer-group:** bin/kafka-consumer-groups.sh --bootstrap-server 10.204.216.10:9092 --describe --group <group-name>

**To see all messages in Kafka topic:** ../bin/kafka-console-consumer.sh --bootstrap-server 10.204.216.10:9092 --topic -uve-0 --partition 0 --property print.key=true

##### 3.3 List relevant alarmgen introspect commands and expected outputs

1. Curl *http://10.204.216.10:8081/analytics/alarms*
   This will give a list of all alarms in the system.

2. Curl http://10.204.216.10:5995/Snh_SandeshUVECacheReq?x=NodeStatus

   This will show whether all connections to Alarmgen are UP or not. All connections should be UP in order for Alarmgen to work properly.

3. Curl http://10.204.216.10:5995/Snh_SandeshUVECacheReq?x=AlarmgenStatus

​   It will give the info about how many partitions allocated to current alarmgen.

4. Curl http://10.204.216.10:5995/Snh_SandeshUVECacheReq?x=AlarmgenPartition

   It will list all partition that are allocated to Alarmgen.

##### 3.4 List relevant logs

Following logs can be collected incase of any alarmgen issue.

1. Contrail logs present under /var/log/contrail
2. Zookeeper docker logs. For example, exec **config_database_zookeeper_1**
3. Kafka logs present under kafka docker. For example:-

​   Goto Kafka docker: **docker exec -it analytics_alarm_kafka_1 bash**

​   Collect all logs present under **/var/log/kafka**
