# Cassandra debugging in Analytics

### 1. Introduction

------

In this document, explains about what do collect in case of any cassandra issues.

### 2. Problem Statement

------

There are cases where OOM issues is being seen in Cassandra very frequently

### 3. Solution

------

#### 3.1 Try solution present in Juniper site

https://kb.juniper.net/InfoCenter/index?page=content&id=KB35008

#### 3.2 List of output of commands to collect in case OOM issue

Commands to collect in db nodes:-

1. contrail-version
2. contrail-status
3. ntpq -p
4. uptime
5. ifconfig
6. uname -a
7. cat /etc/redhat-release
8. free -h
9. df -h
10. lscpu
11. lsof
12. top -n 30 -b
13. ulimit -a curl [http://<node-ip>:8081/analytics/alarms](http://10.84.29.4:8081/analytics/alarms) | python -mjson.tool
14. curl [http://<node-ip>:8081/analytics/uves/analytics-node/<analytics-node-hostname>?flat](http://10.84.29.4:8081/analytics/uves/analytics-node/b7s4.englab.juniper.net?flat) | python -mjson.tool
15. curl [http://<node-ip>:8081/analytics/uves/config-database-node/<config-node-hostname>?flat](http://10.84.29.4:8081/analytics/uves/config-database-node/b7s4.englab.juniper.net?flat) | python -mjson.tool

**Things to collect from inside cassandra docker:-**

16. cqlsh --version
17. cat /etc/cassandra/cassandra.yaml | grep “listen_address:”
18. cqlsh $listen_address
19. cqlsh> SELECT * FROM system_schema.keyspaces;
20. nodetool -p <JMX_PORT> info
21. nodetool -p <JMX_PORT> compactionstats
22. nodetool -p <JMX_PORT> tpstats
23. nodetool -p <JMX_PORT> gcstats
24. nodetool -p <JMX_PORT> netstats
25. nodetool -p <JMX_PORT> ring
26. nodetool -p <JMX_PORT> gossipinfo
27. nodetool -p <JMX_PORT> statusgossip
28. nodetool -p <JMX_PORT> describecluster
29. nodetool -p <JMX_PORT> tablestats; sleep 1m; nodetool -p <JMX_PORT> tablestats
30. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql messagetablev2
31. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql objectvaluetable
32. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql sessiontable
33. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql statstablev4
34. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql systemobjecttable
35. nodetool -p <JMX_PORT> proxyhistograms
36. du -a /var/lib/cassandra | sort -n -r | head -n 10
37. ps -aux | grep cass

#### 3.3 List relevant logs

Following logs can be collected incase of any alarmgen issue.

1. All logs present under **/var/log/cassandra** inside cassandra docker.

#### 3.4  If the above solution doesn't work then check the GC logs

Check the GC log present under /var/log/cassandra inside cassandra container. By default we use Concurrent Mark and Sweep GC and GC format is this :-

{**Heap before GC invocations**=5261 (full 8):

 par new generation  total 368640K, used 327694K [0x00000005cc400000, 0x00000005e5400000, 0x00000005e5400000)

#####  <u>eden space 327680K, 100% used [0x00000005cc400000, 0x00000005e0400000, 0x00000005e0400000)</u>

 **from space 40960K,  0% used** [0x00000005e2c00000, 0x00000005e2c03930, 0x00000005e5400000)

 to  space 40960K,  0% used [0x00000005e0400000, 0x00000005e0400000, 0x00000005e2c00000)

 concurrent mark-sweep generation total 7778304K, used 3687033K [0x00000005e5400000, 0x00000007c0000000, 0x00000007c0000000)

 Metaspace    used 46329K, capacity 47930K, committed 48192K, reserved 1091584K

 class space  used 5740K, capacity 6075K, committed 6212K, reserved 1048576K

2020-05-01T14:33:15.444+0000: 369182.572: [GC (Allocation Failure) 2020-05-01T14:33:15.444+0000: 369182.572: [ParNew

Desired survivor size 20971520 bytes, new threshold 1 (max 1)

\- age  1:   61968 bytes,   61968 total

##### <u>*: 327694K->64K(368640K), 0.0113259 secs] 4014728K->3687107K(8146944K), 0.0114938 secs] [Times: user=0.04 sys=0.00, real=0.01 secs]*</u>

**Heap after GC invocations**=5262 (full 8):

 par new generation  total 368640K, used 64K [0x00000005cc400000, 0x00000005e5400000, 0x00000005e5400000)

 eden space 327680K,  0% used [0x00000005cc400000, 0x00000005cc400000, 0x00000005e0400000)

 from space 40960K,  0% used [0x00000005e0400000, 0x00000005e04102e0, 0x00000005e2c00000)

 to  space 40960K,  0% used [0x00000005e2c00000, 0x00000005e2c00000, 0x00000005e5400000)

 concurrent mark-sweep generation total 7778304K, used 3687043K [0x00000005e5400000, 0x00000007c0000000, 0x00000007c0000000)

 Metaspace    used 46329K, capacity 47930K, committed 48192K, reserved 1091584K

 class space  used 5740K, capacity 6075K, committed 6212K, reserved 1048576K

}



If you see this log:- <u>*: 327694K->64K(368640K), 0.0113259 secs] 4014728K->3687107K(8146944K), 0.0114938 secs] [Times: user=0.04 sys=0.00, real=0.01 secs]*</u>

Total memory is 8146944K and before GC size of heap that has been used is 4014728K and after GC it is 3687107K. That means GC is working. If after GC value is same as previous then GC is not working. Then we have to change the GC method to G1.

If you refer this document https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/operations/opsConHeapSize.html

then you will see that MAX HEAP SIZE for Concurrent Mark and Sweep GC method can be upto 16 GB. So there is another case where load on analyticsdb node is high and current MAX_HEAP_SIZE is not enough to process the data then in that case we have to increase the heap size. For that also we have to change the GC method because CMS GC can work upto 16 GB of MAX_HEAP_SIZE. 

Here is the steps to change the GC method to G1:-

**/etc/cassandra/cassandra-env.sh**

==============================

< MAX_HEAP_SIZE="16G"

< HEAP_NEWSIZE="2G"

\---------------

\> #MAX_HEAP_SIZE="16G"

\> #HEAP_NEWSIZE="2G"

**/etc/cassandra/jvm.options**

==========================

< #-Xms4G

< #-Xmx4G

\----------------

\> -Xms16G             

\> -Xms16G

> >>>> This can go upto 32 GB if the load on the node is high.  https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/operations/opsConHeapSize.html

----------------

< -XX:+UseParNewGC

< -XX:+UseConcMarkSweepGC

< -XX:+CMSParallelRemarkEnabled

< -XX:SurvivorRatio=8

< -XX:MaxTenuringThreshold=1

< -XX:CMSInitiatingOccupancyFraction=75

< -XX:+UseCMSInitiatingOccupancyOnly

< -XX:CMSWaitDuration=10000

< -XX:+CMSParallelInitialMarkEnabled

< -XX:+CMSEdenChunksRecordAlways

------------------

\> #-XX:+UseParNewGC

\> #-XX:+UseConcMarkSweepGC

\> #-XX:+CMSParallelRemarkEnabled

\> #-XX:SurvivorRatio=8

\> #-XX:MaxTenuringThreshold=1

\> #-XX:CMSInitiatingOccupancyFraction=75

\> #-XX:+UseCMSInitiatingOccupancyOnly

\> #-XX:CMSWaitDuration=10000

\> #-XX:+CMSParallelInitialMarkEnabled

\> #-XX:+CMSEdenChunksRecordAlways

-------------------

< -XX:+CMSClassUnloadingEnabled

\----------------

\> #-XX:+CMSClassUnloadingEnabled

======================

< #-XX:+UseG1GC

\--------------------------

\> -XX:+UseG1GC

==========================================

< #-XX:G1RSetUpdatingPauseTimePercent=5

\--------------------------------

\> -XX:G1RSetUpdatingPauseTimePercent=5

==========================================

< #-XX:MaxGCPauseMillis=500

\---------------------------------

\> -XX:MaxGCPauseMillis=500

**Restart the container after making these changes.**