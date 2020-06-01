# Cassandra OOM debugging

### 1. Introduction

------

In this document, explains about what do collect in case of cassandra OOM issues.

### 2. Problem Statement

------

There are cases where OOM issues is being seen in Cassandra very frequently

### 3. Possible Causes

------

By default we defined MAX_HEAP_SIZE to 8GB and HEAP_NEW_SIZE to 8GB. CMS has some drawbacks and GC does not work and it leads to OOM issue. CMS is deprecated in JDK 9. In that case we have to move to other GC method that is G1. In some cases, load in cluster is very high and default MAX_HEAP_SIZE is not sufficient to sustain the load. In that case we have to increase HEAP_SIZE and for that we have to change the GC method G1 because CMS GC supports HEAP_SIZE upto 16GB

### **4. Commands and logs**

------

#### 4.1 Commands to collect from all analytics db nodes:-

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
11. top -n 30 -b
12. ulimit -a curl [http://<node-ip>:8081/analytics/alarms](http://10.84.29.4:8081/analytics/alarms) | python -mjson.tool
13. curl [http://<node-ip>:8081/analytics/uves/analytics-node/<analytics-node-hostname>?flat](http://10.84.29.4:8081/analytics/uves/analytics-node/b7s4.englab.juniper.net?flat) | python -mjson.tool
14. curl [http://<node-ip>:8081/analytics/uves/config-database-node/<config-node-hostname>?flat](http://10.84.29.4:8081/analytics/uves/config-database-node/b7s4.englab.juniper.net?flat) | python -mjson.tool

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
25. nodetool -p <JMX_PORT> gossipinfo
26. nodetool -p <JMX_PORT> statusgossip
27. nodetool -p <JMX_PORT> describecluster
28. nodetool -p <JMX_PORT> tablestats; sleep 1m; nodetool -p <JMX_PORT> tablestats
29. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql messagetablev2
30. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql objectvaluetable
31. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql sessiontable
32. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql statstablev4
33. nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql systemobjecttable
34. nodetool -p <JMX_PORT> proxyhistograms
35. du -a /var/lib/cassandra | sort -n -r | head -n 10
36. ps -aux | grep cass

##### 4.1.1 Adding some working outputs of above commands.

1. ###### nodetool -p <JMX_PORT> info

   ​     Provides node information, including the token and on disk storage (load) information, times started (generation), uptime in seconds, and heap memory usage. https://docs.datastax.com/en/dse/6.0/dse-dev/datastax_enterprise/tools/nodetool/toolsInfo.html

   Sample working O/p

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 info
   ID                     : 9eaf3011-fa89-4d20-90f6-f0d74467bc7c
   Gossip active          : true
   Thrift active          : true
   Native Transport active: true
   Load                   : 1.58 GiB
   Generation No          : 1589188435
   Uptime (seconds)       : 1291080
   Heap Memory (MB)       : 3473.76 / 7962.00
   Off Heap Memory (MB)   : 5.56
   Data Center            : datacenter1
   Rack                   : rack1
   Exceptions             : 0
   Key Cache              : entries 66605, size 5.88 MiB, capacity 100 MiB, 59453 hits, 143017 requests, 0.416 recent hit rate, 14400 save period in seconds
   Row Cache              : entries 0, size 0 bytes, capacity 0 bytes, 0 hits, 0 requests, NaN recent hit rate, 0 save period in seconds
   Counter Cache          : entries 0, size 0 bytes, capacity 50 MiB, 0 hits, 0 requests, NaN recent hit rate, 7200 save period in seconds
   Chunk Cache            : entries 8, size 512 KiB, capacity 480 MiB, 967995 misses, 1281045 requests, 0.244 recent hit rate, NaN microseconds miss latency
   Percent Repaired       : 0.0%
   Token                  : (invoke with -T/--tokens to see all 256 tokens)

   ```
2. ###### nodetool -p <JMX_PORT> compactionstats

   ​      Provide statistics about a compaction. https://docs.datastax.com/en/dse/5.1/dse-admin/datastax_enterprise/tools/nodetool/toolsCompactionStats.html

    `root@pankaj-all-in-one-node1:/# nodetool -p 7200 compactionstats`

   ` pending tasks: 0`

3. ###### nodetool -p <JMX_PORT> tpstats

      Provides usage statistics of thread pools. https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/tools/toolsTPstats.html

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 tpstats
   Pool Name                         Active   Pending      Completed   Blocked  All time blocked
   ReadStage                              0         0         527461         0                 0
   MiscStage                              0         0              0         0                 0
   CompactionExecutor                     0         0         865104         0                 0
   MutationStage                          0         0       19433976         0                 0
   GossipStage                            0         0              0         0                 0
   RequestResponseStage                   0         0              0         0                 0
   ReadRepairStage                        0         0              0         0                 0
   CounterMutationStage                   0         0              0         0                 0
   MemtablePostFlush                      0         0           1146         0                 0
   SASI-Memtable                          0         0           1295         0                 0
   ValidationExecutor                     0         0              0         0                 0
   MemtableFlushWriter                    0         0           1124         0                 0
   ViewMutationStage                      0         0              0         0                 0
   SASI-General                           0         0            521         0                 0
   CacheCleanupExecutor                   0         0              0         0                 0
   MemtableReclaimMemory                  0         0           1124         0                 0
   PendingRangeCalculator                 0         0              1         0                 0
   SecondaryIndexManagement               0         0              0         0                 0
   HintsDispatcher                        0         0              0         0                 0
   Native-Transport-Requests              0         0       18894477         0                 0
   MigrationStage                         0         0              0         0                 0
   PerDiskMemtableFlushWriter_0           0         0           1124         0                 0
   Sampler                                0         0              0         0                 0
   InternalResponseStage                  0         0              0         0                 0
   AntiEntropyStage                       0         0              0         0                 0

   Message type           Dropped
   READ                         0
   RANGE_SLICE                  0
   _TRACE                       0
   HINT                         0
   MUTATION                     0
   COUNTER_MUTATION             0
   BATCH_STORE                  0
   BATCH_REMOVE                 0
   REQUEST_RESPONSE             0
   PAGED_RANGE                  0
   READ_REPAIR                  0
   ```

4. ###### nodetool -p <JMX_PORT> gcstats

   Prints garbage collection statistics that returns values based on all the garbage collection that has run since the last time this command was run. Statistics identify the interval, GC elapsed time (total and standard deviation), the disk space reclaimed in megabytes (MB), number of garbage collections, and direct memory bytes. https://docs.datastax.com/en/dse/6.0/dse-dev/datastax_enterprise/tools/nodetool/toolsGcstats.html

   > root@pankaj-all-in-one-node1:/# nodetool -p 7200 gcstats
   >
   > ​    Interval (ms) Max GC Elapsed (ms)Total GC Elapsed (ms)Stdev GC Elapsed (ms)  GC Reclaimed (MB)     Collections   Direct Memory Bytes
   >
   > ​     1296324434         824       695421         40    3805276884616
   > 11377      -1                                                                                          

5. ###### nodetool -p <JMX_PORT> netstats

   Returns network information about the host. https://docs.datastax.com/en/dse/6.7/dse-dev/datastax_enterprise/tools/nodetool/toolsNetstats.html

   Results include the following information:

   - Mode - The operational mode of the node: JOINING, LEAVING, NORMAL, DECOMMISSIONED, CLIENT.
   - Read repair statistics
     - Attempted - The number of successfully completed [read repair operations](https://docs.datastax.com/en/dse/6.7/dse-arch/datastax_enterprise/dbInternals/dbIntClientRequestsRead.html).
     - Mismatch (blocking) - The number of read repair operations since server restart that blocked a query.
     - Mismatch (background) - The number of read repair operations since server restart performed in the background.
   - Pool name - Information about client read and write requests by thread pool size.
   - Active, pending, completed, and dropped number of commands and responses for large, small, and gossip messages.

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 netstats
   Mode: NORMAL
   Not sending any streams.
   Read Repair Statistics:
   Attempted: 26888
   Mismatch (Blocking): 0
   Mismatch (Background): 0
   Pool Name                    Active   Pending      Completed   Dropped
   Large messages                  n/a         0              0         0
   Small messages                  n/a         0              0         0
   Gossip messages                 n/a         0              0         0
   ```

6. ###### nodetool -p <JMX_PORT> gossipinfo

   Shows the gossip information to discover broadcast protocol between nodes in a cluster. https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/tools/nodetool/toolsGossipInfo.html

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 gossipinfo
   /10.204.220.26
     generation:1589188435
     heartbeat:1338944
     STATUS:15:NORMAL,-1046396384841236499
     LOAD:1338942:1.713105305E9
     SCHEMA:11:246c1a64-a67c-38b3-ad44-c7faffa0b561
     DC:7:datacenter1
     RACK:9:rack1
     RELEASE_VERSION:5:3.11.5
     RPC_ADDRESS:4:10.204.220.26
     NET_VERSION:2:11
     HOST_ID:3:9eaf3011-fa89-4d20-90f6-f0d74467bc7c
     RPC_READY:27:true
     TOKENS:14:<hidden>
   ```

7. ###### nodetool -p <JMX_PORT> statusgossip

   Provides status of gossip. https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/tools/nodetool/toolsStatusGossip.html

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 statusgossip
   running
   ```

8. ###### nodetool -p <JMX_PORT> describecluster

   Prints the name, snitch, partitioner and schema version of a cluster. https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/tools/nodetool/toolsDescribeCluster.html

   ```
   root@pankaj-all-in-one-node1:/# nodetool -p 7200 describecluster
   Cluster Information:
   	Name: contrail_analytics
   	Snitch: org.apache.cassandra.locator.SimpleSnitch
   	DynamicEndPointSnitch: enabled
   	Partitioner: org.apache.cassandra.dht.Murmur3Partitioner
   	Schema versions:
   		246c1a64-a67c-38b3-ad44-c7faffa0b561: [10.204.220.26]
   ```

9. ###### nodetool -p <JMX_PORT> tablestats

   Provides statistics about one or more tables. https://docs.datastax.com/en/dse/5.1/dse-admin/datastax_enterprise/tools/nodetool/toolsTablestats.html

   ```
   ----------------
   Keyspace : ContrailAnalyticsCql
   	Read Count: 267721
   	Read Latency: 1.0224852140848122 ms
   	Write Count: 18330981
   	Write Latency: 0.10932457837362879 ms
   	Pending Flushes: 0
   		Table: messagetablev2
   		SSTable count: 6
   		Space used (live): 898686140
   		Space used (total): 898686140
   		Space used by snapshots (total): 0
   		Off heap memory used (total): 2136280
   		SSTable Compression Ratio: 0.11173804976493089
   		Number of partitions (estimate): 500881
   		Memtable cell count: 13292
   		Memtable data size: 94925781
   		Memtable off heap memory used: 0
   		Memtable switch count: 56
   		Local read count: 792
   		Local read latency: NaN ms
   		Local write count: 1908986
   		Local write latency: 0.098 ms
   		Pending flushes: 0
   		Percent repaired: 0.0
   		Bloom filter false positives: 0
   		Bloom filter false ratio: 0.00000
   		Bloom filter space used: 1069584
   		Bloom filter off heap memory used: 1069536
   		Index summary off heap memory used: 99632
   		Compression metadata off heap memory used: 967112
   		Compacted partition minimum bytes: 61
   		Compacted partition maximum bytes: 182785
   		Compacted partition mean bytes: 17633
   		Average live cells per slice (last five minutes): NaN
   		Maximum live cells per slice (last five minutes): 0
   		Average tombstones per slice (last five minutes): NaN
   		Maximum tombstones per slice (last five minutes): 0
   		Dropped Mutations: 0
   
   		Table: objectvaluetable
   		SSTable count: 2
   		Space used (live): 18047606
   		Space used (total): 18047606
   		Space used by snapshots (total): 0
   		Off heap memory used (total): 297411
   		SSTable Compression Ratio: 0.2510365239832307
   		Number of partitions (estimate): 182700
   		Memtable cell count: 89202
   		Memtable data size: 9125868
   		Memtable off heap memory used: 0
   		Memtable switch count: 4
   		Local read count: 0
   		Local read latency: NaN ms
   		Local write count: 2032161
   		Local write latency: 0.032 ms
   		Pending flushes: 0
   		Percent repaired: 0.0
   		Bloom filter false positives: 0
   		Bloom filter false ratio: 0.00000
   		Bloom filter space used: 242552
   		Bloom filter off heap memory used: 242536
   		Index summary off heap memory used: 48875
   		Compression metadata off heap memory used: 6000
   		Compacted partition minimum bytes: 61
   		Compacted partition maximum bytes: 3973
   		Compacted partition mean bytes: 329
   		Average live cells per slice (last five minutes): NaN
   		Maximum live cells per slice (last five minutes): 0
   		Average tombstones per slice (last five minutes): NaN
   		Maximum tombstones per slice (last five minutes): 0
   		Dropped Mutations: 0
   
   		Table: sessiontable
   		SSTable count: 0
   		Space used (live): 0
   		Space used (total): 0
   		Space used by snapshots (total): 0
   		Off heap memory used (total): 0
   		SSTable Compression Ratio: -1.0
   		Number of partitions (estimate): 0
   		Memtable cell count: 0
   		Memtable data size: 0
   		Memtable off heap memory used: 0
   		Memtable switch count: 0
   		Local read count: 0
   		Local read latency: NaN ms
   		Local write count: 0
   		Local write latency: NaN ms
   		Pending flushes: 0
   		Percent repaired: 100.0
   		Bloom filter false positives: 0
   		Bloom filter false ratio: 0.00000
   		Bloom filter space used: 0
   		Bloom filter off heap memory used: 0
   		Index summary off heap memory used: 0
   		Compression metadata off heap memory used: 0
   		Compacted partition minimum bytes: 0
   		Compacted partition maximum bytes: 0
   		Compacted partition mean bytes: 0
   		Average live cells per slice (last five minutes): NaN
   		Maximum live cells per slice (last five minutes): 0
   		Average tombstones per slice (last five minutes): NaN
   		Maximum tombstones per slice (last five minutes): 0
   		Dropped Mutations: 0
   
   		Table: statstablev4
   		SSTable count: 4
   		Space used (live): 795996560
   		Space used (total): 795996560
   		Space used by snapshots (total): 0
   		Off heap memory used (total): 3445123
   		SSTable Compression Ratio: 0.12162223030312344
   		Number of partitions (estimate): 930447
   		Memtable cell count: 23689
   		Memtable data size: 19612793
   		Memtable off heap memory used: 0
   		Memtable switch count: 113
   		Local read count: 266927
   		Local read latency: NaN ms
   		Local write count: 14389833
   		Local write latency: 0.086 ms
   		Pending flushes: 0
   		Percent repaired: 0.0
   		Bloom filter false positives: 0
   		Bloom filter false ratio: 0.00000
   		Bloom filter space used: 2264632
   		Bloom filter off heap memory used: 2264600
   		Index summary off heap memory used: 444859
   		Compression metadata off heap memory used: 735664
   		Compacted partition minimum bytes: 311
   		Compacted partition maximum bytes: 152321
   		Compacted partition mean bytes: 7037
   		Average live cells per slice (last five minutes): NaN
   		Maximum live cells per slice (last five minutes): 0
   		Average tombstones per slice (last five minutes): NaN
   		Maximum tombstones per slice (last five minutes): 0
   		Dropped Mutations: 0
   
   		Table: systemobjecttable
   		SSTable count: 2
   		Space used (live): 10658
   		Space used (total): 10658
   		Space used by snapshots (total): 0
   		Off heap memory used (total): 98
   		SSTable Compression Ratio: 0.8290155440414507
   		Number of partitions (estimate): 1
   		Memtable cell count: 0
   		Memtable data size: 0
   		Memtable off heap memory used: 0
   		Memtable switch count: 1
   		Local read count: 2
   		Local read latency: NaN ms
   		Local write count: 1
   		Local write latency: NaN ms
   		Pending flushes: 0
   		Percent repaired: 0.0
   		Bloom filter false positives: 0
   		Bloom filter false ratio: 0.00000
   		Bloom filter space used: 32
   		Bloom filter off heap memory used: 16
   		Index summary off heap memory used: 66
   		Compression metadata off heap memory used: 16
   		Compacted partition minimum bytes: 73
   		Compacted partition maximum bytes: 124
   		Compacted partition mean bytes: 105
   		Average live cells per slice (last five minutes): NaN
   		Maximum live cells per slice (last five minutes): 0
   		Average tombstones per slice (last five minutes): NaN
   		Maximum tombstones per slice (last five minutes): 0
   		Dropped Mutations: 0
   ```

10. ###### nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql messagetablev2

    Initial troubleshooting and performance metrics that provide current performance statics for read and write latency on a table during the past 15 minutes. https://docs.datastax.com/en/dse/6.0/dse-dev/datastax_enterprise/tools/nodetool/toolsTablehistograms.html

    > root@pankaj-all-in-one-node1:/# nodetool -p 7200 tablehistograms ContrailAnalyticsCql messagetablev2
    >
    > ContrailAnalyticsCql/messagetablev2 histograms
    >
    > Percentile SSTables   Write Latency   Read Latency  Partition Size    Cell Count
    >
    > ​                                        (micros)              (micros)      (bytes)          
    >
    > 50%                                     0.00       88.15       0.00       8239                 24
    >
    > 75%                                     0.00      152.32       0.00       29521             35
    >
    > 95%                                     0.00      219.34       0.00       61214            72
    >
    > 98%                                     0.00      263.21       0.00       73457           86
    >
    > 99%                                     0.00      315.85       0.00       88148         103
    >
    > Min                                     0.00       24.60       0.00        61                  0
    >
    > Max                                    0.00      379.02       0.00      182785        179

    All the above values, they show the distribution of the metrics. 

    For example, in your data the 

    * write latency for 95% of the requests were 219.34 microseconds or less. 
    * 95% of the partitions are 61214 bytes or less with 72 cells. 
    * The SStables column is how many sstables are touched on a read, so 95% or read requests are looking at 0 sstables.

11. ###### nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql objectvaluetable

    Same explanation as above just it is for a different table.

12. ###### nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql sessiontable

    Same explanation as above just it is for a different table.

13. ###### nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql statstablev4

    Same explanation as above just it is for a different table.

14. ###### nodetool -p <JMX_PORT> tablehistograms ContrailAnalyticsCql systemobjecttable

    Same explanation as above just it is for a different table.

15. ###### nodetool -p <JMX_PORT> proxyhistograms

    > root@pankaj-all-in-one-node1:/# nodetool -p 7200 proxyhistograms
    >
    > proxy histograms
    >
    > Percentile    Read Latency   Write Latency   Range Latency  CAS Read Latency CAS Write Latency View Write Latency
    >
    > ​           (micros)      (micros)      (micros)      (micros)      (micros)      (micros)
    >
    > 50%          545.79       219.34       943.13        0.00        0.00        0.00
    >
    > 75%          943.13       654.95      2346.80        0.00        0.00        0.00
    >
    > 95%          943.13      1955.67      4866.32        0.00        0.00        0.00
    >
    > 98%          943.13      2816.16      4866.32        0.00        0.00        0.00
    >
    > 99%          943.13      4055.27      4866.32        0.00        0.00        0.00
    >
    > Min          454.83        9.89       182.79        0.00        0.00        0.00
    >
    > Max          943.13      7007.51      4866.32        0.00        0.00        0.00

#### 4.2 List relevant logs

Following logs can be collected incase of any Cassandra OOM issue.

1. All logs present under **/var/log/cassandra** inside cassandra docker.
2. Also copy cassandra-env.sh and jvm.options and cassandra.yaml file present under /etc/cassandra inside cassandra container.

### 5. Solution

------

#### 5.1 Try solution present in Juniper site

https://kb.juniper.net/InfoCenter/index?page=content&id=KB35008

#### 5.2  If the above solution doesn't work then check the GC logs

Check the GC log *(gc.log.*.current needs to be checked) present under /var/log/cassandra inside cassandra container. By default we use Concurrent Mark and Sweep GC and GC format is this :-

**Note:-** If you have to check GC recent logs then open file with **.current** extension

> {**Heap before GC invocations**=5261 (full 8):
>
>  par new generation  total 368640K, used 327694K [0x00000005cc400000, 0x00000005e5400000, 0x00000005e5400000)
>
> #####  <u>eden space 327680K, 100% used [0x00000005cc400000, 0x00000005e0400000, 0x00000005e0400000)</u>
>
>  **from space 40960K,  0% used** [0x00000005e2c00000, 0x00000005e2c03930, 0x00000005e5400000)
>
>  to  space 40960K,  0% used [0x00000005e0400000, 0x00000005e0400000, 0x00000005e2c00000)
>
>  concurrent mark-sweep generation total 7778304K, used 3687033K [0x00000005e5400000, 0x00000007c0000000, 0x00000007c0000000)
>
>  Metaspace    used 46329K, capacity 47930K, committed 48192K, reserved 1091584K
>
>  class space  used 5740K, capacity 6075K, committed 6212K, reserved 1048576K
>
> 2020-05-01T14:33:15.444+0000: 369182.572: [GC (Allocation Failure) 2020-05-01T14:33:15.444+0000: 369182.572: [ParNew
>
> Desired survivor size 20971520 bytes, new threshold 1 (max 1)
>
> \- age  1:   61968 bytes,   61968 total
>
> ##### <u>*: 327694K->64K(368640K), 0.0113259 secs] 4014728K->3687107K(8146944K), 0.0114938 secs] [Times: user=0.04 sys=0.00, real=0.01 secs]*</u>
>
> **Heap after GC invocations**=5262 (full 8):
>
>  par new generation  total 368640K, used 64K [0x00000005cc400000, 0x00000005e5400000, 0x00000005e5400000)
>
>  eden space 327680K,  0% used [0x00000005cc400000, 0x00000005cc400000, 0x00000005e0400000)
>
>  from space 40960K,  0% used [0x00000005e0400000, 0x00000005e04102e0, 0x00000005e2c00000)
>
>  to  space 40960K,  0% used [0x00000005e2c00000, 0x00000005e2c00000, 0x00000005e5400000)
>
>  concurrent mark-sweep generation total 7778304K, used 3687043K [0x00000005e5400000, 0x00000007c0000000, 0x00000007c0000000)
>
>  Metaspace    used 46329K, capacity 47930K, committed 48192K, reserved 1091584K
>
>  class space  used 5740K, capacity 6075K, committed 6212K, reserved 1048576K
>
> }

If you see this log:- <u>*: **327694K->64K(368640K), 0.0113259 secs] 4014728K->3687107K(8146944K), 0.0114938 secs] [Times: user=0.04 sys=0.00, real=0.01 secs]*</u>**

Total memory is 8146944K and before GC size of heap that has been used is 4014728K and after GC it is 3687107K. That means GC is working. If after GC value is same as previous then GC is not working. Then we have to change the GC method to G1.

If you refer this document https://docs.datastax.com/en/dse/6.0/dse-admin/datastax_enterprise/operations/opsConHeapSize.html

then you will see that MAX HEAP SIZE for Concurrent Mark and Sweep GC method can be upto 16 GB. So there is another case where load on analyticsdb node is high and current MAX_HEAP_SIZE is not enough to process the data then in that case we have to increase the heap size. For that also we have to change the GC method because CMS GC can work upto 16 GB of MAX_HEAP_SIZE.

Here is the steps to change the GC method to G1:-

```
/etc/cassandra/cassandra-env.sh**

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


```

**Restart the container after making these changes.**



#### 5.3 How to check if G1GC is working or not.

Sample G1 GC log:-

> {Heap before GC invocations=0 (full 0):
>
>  garbage-first heap  total 33554432K, used 147456K [0x00007f40e0000000, 0x00007f40e1004000, 0x00007f48e0000000)
>
>  region size 16384K, 10 young (163840K), 0 survivors (0K)
>
>  Metaspace    used 20699K, capacity 21093K, committed 21248K, reserved 22528K
>
> 2020-05-04T13:23:52.009+1000: 17.214: [GC pause (Metadata GC Threshold) (young) (initial-mark)
>
> Desired survivor size 109051904 bytes, new threshold 15 (max 15)
>
> , 0.0429396 secs]
>
>   [Parallel Time: 20.7 ms, GC Workers: 10]
>
>    [GC Worker Start (ms): Min: 17214.3, Avg: 17218.5, Max: 17220.2, Diff: 5.9]
>
>    [Ext Root Scanning (ms): Min: 0.1, Avg: 13.7, Max: 17.4, Diff: 17.3, Sum: 137.3]
>
>    [Update RS (ms): Min: 0.0, Avg: 0.0, Max: 0.0, Diff: 0.0, Sum: 0.0]
>
> ​     [Processed Buffers: Min: 0, Avg: 0.0, Max: 0, Diff: 0, Sum: 0]
>
>    [Scan RS (ms): Min: 0.0, Avg: 0.0, Max: 0.0, Diff: 0.0, Sum: 0.0]
>
>    [Code Root Scanning (ms): Min: 0.0, Avg: 0.1, Max: 1.0, Diff: 1.0, Sum: 1.0]
>
>    [Object Copy (ms): Min: 0.0, Avg: 2.1, Max: 12.9, Diff: 12.9, Sum: 20.5]
>
>    [Termination (ms): Min: 0.0, Avg: 0.2, Max: 0.4, Diff: 0.4, Sum: 2.4]
>
> ​     [Termination Attempts: Min: 1, Avg: 3.7, Max: 18, Diff: 17, Sum: 37]
>
>    [GC Worker Other (ms): Min: 0.0, Avg: 0.0, Max: 0.0, Diff: 0.0, Sum: 0.2]
>
>    [GC Worker Total (ms): Min: 14.4, Avg: 16.1, Max: 20.3, Diff: 5.9, Sum: 161.4]
>
>    [GC Worker End (ms): Min: 17234.7, Avg: 17234.7, Max: 17234.7, Diff: 0.0]
>
>   [Code Root Fixup: 0.1 ms]
>
>   [Code Root Purge: 0.0 ms]
>
>   [Clear CT: 0.5 ms]
>
>   [Other: 21.7 ms]
>
>    [Choose CSet: 0.0 ms]
>
>    [Ref Proc: 20.5 ms]
>
>    [Ref Enq: 0.4 ms]
>
>    [Redirty Cards: 0.3 ms]
>
>    [Humongous Register: 0.0 ms]
>
>    [Humongous Reclaim: 0.0 ms]
>
>    [Free CSet: 0.1 ms]
>
>   **<u>*[Eden: 160.0M(1632.0M)->0.0B(1616.0M) Survivors: 0.0B->16.0M Heap: 152.0M(32.0G)->15.6M(32.0G)]*</u>**
>
> Heap after GC invocations=1 (full 0):
>
>  garbage-first heap  total 33554432K, used 15986K [0x00007f40e0000000, 0x00007f40e1004000, 0x00007f48e0000000)
>
>  region size 16384K, 1 young (16384K), 1 survivors (16384K)
>
>  Metaspace    used 20699K, capacity 21093K, committed 21248K, reserved 22528K
>
> }
>



If you see the output :-

  **<u>*[Eden: 160.0M(1632.0M)->0.0B(1616.0M) Survivors: 0.0B->16.0M Heap: 152.0M(32.0G)->15.6M(32.0G)]*</u>**

It says that before GC heap was 152.0 MB and after GC invocations heap become 15.6 MB. That means GC is working properly.
