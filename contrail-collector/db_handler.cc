/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <exception>
#include <string>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/uuid/name_generator.hpp>
#include <boost/system/error_code.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <base/string_util.h>
#include <base/logging.h>
#include <io/event_manager.h>
#include <base/connection_info.h>
#include <base/address_util.h>
#include <sandesh/sandesh_message_builder.h>
#include <sandesh/protocol/TXMLProtocol.h>
#include <database/cassandra/cql/cql_if.h>
#include <zookeeper/zookeeper_client.h>

#include "sandesh/common/flow_constants.h"
#include "sandesh/common/flow_types.h"
#include "viz_constants.h"
#include "vizd_table_desc.h"
#include "viz_collector.h"
#include "collector.h"
#include "db_handler.h"
#include "parser_util.h"
#include "db_handler_impl.h"
#include "viz_sandesh.h"

#define DB_LOG(_Level, _Msg)                                                   \
    do {                                                                       \
        if (LoggingDisabled()) break;                                          \
        log4cplus::Logger _Xlogger = log4cplus::Logger::getRoot();             \
        if (_Xlogger.isEnabledFor(log4cplus::_Level##_LOG_LEVEL)) {            \
            log4cplus::tostringstream _Xbuf;                                   \
            _Xbuf << name_ << ": " << __func__ << ": " << _Msg;                \
            _Xlogger.forcedLog(log4cplus::_Level##_LOG_LEVEL,                  \
                               _Xbuf.str());                                   \
        }                                                                      \
    } while (false)

using std::pair;
using std::string;
using boost::system::error_code;
using namespace pugi;
using namespace contrail::sandesh::protocol;
using process::ConnectionState;
using process::ConnectionType;
using process::ConnectionStatus;
using namespace boost::system;

uint32_t DbHandler::field_cache_index_ = 0;
std::set<std::string> DbHandler::field_cache_set_;
tbb::mutex DbHandler::fmutex_;

DbHandler::DbHandler(EventManager *evm,
        GenDb::GenDbIf::DbErrorHandler err_handler,
        std::string name,
        const Options::Cassandra &cassandra_options,
        bool use_db_write_options,
        const DbWriteOptions &db_write_options,
        ConfigClientCollector *config_client) :
    dbif_(new cass::cql::CqlIf(evm, cassandra_options.cassandra_ips_,
        cassandra_options.cassandra_ports_[0],
        cassandra_options.user_, cassandra_options.password_,
        cassandra_options.use_ssl_, cassandra_options.ca_certs_,
        true)),
    name_(name),
    drop_level_(SandeshLevel::INVALID),
    ttl_map_(cassandra_options.ttlmap_),
    compaction_strategy_(cassandra_options.compaction_strategy_),
    flow_tables_compaction_strategy_(
        cassandra_options.flow_tables_compaction_strategy_),
    gen_partition_no_((uint8_t)g_viz_constants.PARTITION_MIN,
        (uint8_t)g_viz_constants.PARTITION_MAX),
    disable_all_writes_(cassandra_options.disable_all_db_writes_),
    disable_statistics_writes_(cassandra_options.disable_db_stats_writes_),
    disable_messages_writes_(cassandra_options.disable_db_messages_writes_),
    config_client_(config_client),
    use_db_write_options_(use_db_write_options) {
    udc_.reset(new UserDefinedCounters());
    if (config_client) {
        config_client->RegisterConfigReceive("udc",
                             boost::bind(&DbHandler::ReceiveConfig, this, _1, _2));
    }
    error_code error;
    col_name_ = ResolveCanonicalName();

    if (cassandra_options.cluster_id_.empty()) {
        tablespace_ = g_viz_constants.COLLECTOR_KEYSPACE_CQL;
    } else {
        tablespace_ = g_viz_constants.COLLECTOR_KEYSPACE_CQL + '_' + cassandra_options.cluster_id_;
    }

    if (use_db_write_options_) {
        // Set disk-usage watermark defaults
        SetDiskUsagePercentageHighWaterMark(
            db_write_options.get_disk_usage_percentage_high_watermark0(),
            db_write_options.get_high_watermark0_message_severity_level());
        SetDiskUsagePercentageLowWaterMark(
            db_write_options.get_disk_usage_percentage_low_watermark0(),
            db_write_options.get_low_watermark0_message_severity_level());
        SetDiskUsagePercentageHighWaterMark(
            db_write_options.get_disk_usage_percentage_high_watermark1(),
            db_write_options.get_high_watermark1_message_severity_level());
        SetDiskUsagePercentageLowWaterMark(
            db_write_options.get_disk_usage_percentage_low_watermark1(),
            db_write_options.get_low_watermark1_message_severity_level());
        SetDiskUsagePercentageHighWaterMark(
            db_write_options.get_disk_usage_percentage_high_watermark2(),
            db_write_options.get_high_watermark2_message_severity_level());
        SetDiskUsagePercentageLowWaterMark(
            db_write_options.get_disk_usage_percentage_low_watermark2(),
            db_write_options.get_low_watermark2_message_severity_level());

        // Set cassandra pending tasks watermark defaults
        SetPendingCompactionTasksHighWaterMark(
            db_write_options.get_pending_compaction_tasks_high_watermark0(),
            db_write_options.get_high_watermark0_message_severity_level());
        SetPendingCompactionTasksLowWaterMark(
            db_write_options.get_pending_compaction_tasks_low_watermark0(),
            db_write_options.get_low_watermark0_message_severity_level());
        SetPendingCompactionTasksHighWaterMark(
            db_write_options.get_pending_compaction_tasks_high_watermark1(),
            db_write_options.get_high_watermark1_message_severity_level());
        SetPendingCompactionTasksLowWaterMark(
            db_write_options.get_pending_compaction_tasks_low_watermark1(),
            db_write_options.get_low_watermark1_message_severity_level());
        SetPendingCompactionTasksHighWaterMark(
            db_write_options.get_pending_compaction_tasks_high_watermark2(),
            db_write_options.get_high_watermark2_message_severity_level());
        SetPendingCompactionTasksLowWaterMark(
            db_write_options.get_pending_compaction_tasks_low_watermark2(),
            db_write_options.get_low_watermark2_message_severity_level());

        // Initialize drop-levels to lowest severity level.
        SetDiskUsagePercentageDropLevel(0,
            db_write_options.get_low_watermark2_message_severity_level());
        SetPendingCompactionTasksDropLevel(0,
            db_write_options.get_low_watermark2_message_severity_level());
    }
    session_table_db_stats_ = SessionTableDbStats();
}


DbHandler::DbHandler(GenDb::GenDbIf *dbif, const TtlMap& ttl_map) :
    dbif_(dbif),
    ttl_map_(ttl_map),
    gen_partition_no_((uint8_t)g_viz_constants.PARTITION_MIN,
        (uint8_t)g_viz_constants.PARTITION_MAX),
    disable_all_writes_(false),
    disable_statistics_writes_(false),
    disable_messages_writes_(false),
    use_db_write_options_(false) {
    udc_.reset(new UserDefinedCounters());
}

DbHandler::~DbHandler() {
}

uint64_t DbHandler::GetTtlInHourFromMap(const TtlMap& ttl_map,
        TtlType::type type) {
    TtlMap::const_iterator it = ttl_map.find(type);
    if (it != ttl_map.end()) {
        return it->second;
    } else {
        return 0;
    }
}

uint64_t DbHandler::GetTtlFromMap(const TtlMap& ttl_map,
        TtlType::type type) {
    TtlMap::const_iterator it = ttl_map.find(type);
    if (it != ttl_map.end()) {
        return it->second*3600;
    } else {
        return 0;
    }
}

std::string DbHandler::GetName() const {
    return name_;
}

std::vector<boost::asio::ip::tcp::endpoint> DbHandler::GetEndpoints() const {
    return dbif_->Db_GetEndpoints();
}

void DbHandler::SetDiskUsagePercentageDropLevel(size_t count,
                                    SandeshLevel::type drop_level) {
    disk_usage_percentage_drop_level_ = drop_level;
}

void DbHandler::SetDiskUsagePercentage(size_t disk_usage_percentage) {
    disk_usage_percentage_ = disk_usage_percentage;
}

void DbHandler::SetDiskUsagePercentageHighWaterMark(
                                        uint32_t disk_usage_percentage,
                                        SandeshLevel::type level) {
    WaterMarkInfo wm = WaterMarkInfo(disk_usage_percentage,
        boost::bind(&DbHandler::SetDiskUsagePercentageDropLevel,
                    this, _1, level));
    disk_usage_percentage_watermark_tuple_.SetHighWaterMark(wm);
}

void DbHandler::SetDiskUsagePercentageLowWaterMark(
                                        uint32_t disk_usage_percentage,
                                        SandeshLevel::type level) {
    WaterMarkInfo wm = WaterMarkInfo(disk_usage_percentage,
        boost::bind(&DbHandler::SetDiskUsagePercentageDropLevel,
                    this, _1, level));
    disk_usage_percentage_watermark_tuple_.SetLowWaterMark(wm);
}

void DbHandler::ProcessDiskUsagePercentage(uint32_t disk_usage_percentage) {
    tbb::mutex::scoped_lock lock(disk_usage_percentage_water_mutex_);
    disk_usage_percentage_watermark_tuple_.ProcessWaterMarks(
                                        disk_usage_percentage,
                                        DbHandler::disk_usage_percentage_);
}

void DbHandler::SetPendingCompactionTasksDropLevel(size_t count,
                                    SandeshLevel::type drop_level) {
    pending_compaction_tasks_drop_level_ = drop_level;
}

void DbHandler::SetPendingCompactionTasks(size_t pending_compaction_tasks) {
    pending_compaction_tasks_ = pending_compaction_tasks;
}

void DbHandler::SetPendingCompactionTasksHighWaterMark(
                                        uint32_t pending_compaction_tasks,
                                        SandeshLevel::type level) {
    WaterMarkInfo wm = WaterMarkInfo(pending_compaction_tasks,
        boost::bind(&DbHandler::SetPendingCompactionTasksDropLevel, this,
                    _1, level));
    pending_compaction_tasks_watermark_tuple_.SetHighWaterMark(wm);
}

void DbHandler::SetPendingCompactionTasksLowWaterMark(
                                        uint32_t pending_compaction_tasks,
                                        SandeshLevel::type level) {
    WaterMarkInfo wm = WaterMarkInfo(pending_compaction_tasks,
        boost::bind(&DbHandler::SetPendingCompactionTasksDropLevel, this,
                    _1, level));
    pending_compaction_tasks_watermark_tuple_.SetLowWaterMark(wm);
}

void DbHandler::ProcessPendingCompactionTasks(
                                    uint32_t pending_compaction_tasks) {
    tbb::mutex::scoped_lock lock(pending_compaction_tasks_water_mutex_);
    pending_compaction_tasks_watermark_tuple_.ProcessWaterMarks(
                                    pending_compaction_tasks,
                                    DbHandler::pending_compaction_tasks_);
}

bool DbHandler::DropMessage(const SandeshHeader &header,
    const VizMsg *vmsg) {
    SandeshType::type stype(header.get_Type());
    // If Flow message, drop it
    if (stype == SandeshType::FLOW) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Update(vmsg);
        return true;
    }
    if (!(stype == SandeshType::SYSTEM || stype == SandeshType::OBJECT ||
          stype == SandeshType::UVE ||
          stype == SandeshType::SESSION)) {
        return false;
    }
    // First check again the queue watermark drop level
    SandeshLevel::type slevel(static_cast<SandeshLevel::type>(
        header.get_Level()));
    if (slevel >= drop_level_) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Update(vmsg);
        return true;
    }
    // Next check against the disk usage and pending compaction tasks
    // drop levels
    bool disk_usage_percentage_drop = false;
    bool pending_compaction_tasks_drop = false;
    if (use_db_write_options_) {
        SandeshLevel::type disk_usage_percentage_drop_level =
                                        GetDiskUsagePercentageDropLevel();
        SandeshLevel::type pending_compaction_tasks_drop_level =
                                        GetPendingCompactionTasksDropLevel();
        if (slevel >= disk_usage_percentage_drop_level) {
            disk_usage_percentage_drop = true;
        }
        if (slevel >= pending_compaction_tasks_drop_level) {
            pending_compaction_tasks_drop = true;
        }
    }

    bool drop(disk_usage_percentage_drop || pending_compaction_tasks_drop);
    if (drop) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Update(vmsg);
    }
    return drop;
}
 
void DbHandler::SetDropLevel(size_t queue_count, SandeshLevel::type level,
    boost::function<void (void)> cb) {
    if (drop_level_ != level) {
        DB_LOG(INFO, "DB DROP LEVEL: [" << 
            Sandesh::LevelToString(drop_level_) << "] -> [" <<
            Sandesh::LevelToString(level) << "], DB QUEUE COUNT: " << 
            queue_count);
        drop_level_ = level;
    }
    // Always invoke the callback
    if (!cb.empty()) {
        cb();
    }
}

bool DbHandler::CreateTables() {
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_tables.begin();
            it != vizd_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it, compaction_strategy_)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }

        // find schema for it->cfname_
        // find all columns with index_type field set
        table_schema cfschema;
        cfschema = g_viz_constants._VIZD_TABLE_SCHEMA.find(it->cfname_)->second;
        BOOST_FOREACH (schema_column column, cfschema.columns) {
            if (column.index_mode != GenDb::ColIndexMode::NONE) {
                if (!dbif_->Db_CreateIndex(it->cfname_, column.name, "",
                                           column.index_mode)) {
                    DB_LOG(ERROR, it->cfname_ << ": CreateIndex FAILED for "
                           << column.name);
                    return false;
                }
            }
        }
    }

    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_stat_tables.begin();
            it != vizd_stat_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it, compaction_strategy_)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }
        table_schema cfschema;
        cfschema = g_viz_constants._VIZD_STAT_TABLE_SCHEMA.find(it->cfname_)->second;
        BOOST_FOREACH (schema_column column, cfschema.columns) {
            if (column.index_mode != GenDb::ColIndexMode::NONE) {
                if (!dbif_->Db_CreateIndex(it->cfname_, column.name, "",
                                           column.index_mode)) {
                    DB_LOG(ERROR, it->cfname_ << ": CreateIndex FAILED for "
                           << column.name);
                    return false;
                }
	    }
        }
    }

    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_session_tables.begin();
            it != vizd_session_tables.end(); it++) {
        if (!dbif_->Db_AddColumnfamily(*it, flow_tables_compaction_strategy_)) {
            DB_LOG(ERROR, it->cfname_ << " FAILED");
            return false;
        }

        // will be un-commented once we start using cassandra-2.10 for systemless tests
        table_schema cfschema = g_viz_constants._VIZD_SESSION_TABLE_SCHEMA
                                                    .find(it->cfname_)->second;
        BOOST_FOREACH(schema_column column, cfschema.columns) {
            if (column.index_mode != GenDb::ColIndexMode::NONE) {
                if (!dbif_->Db_CreateIndex(it->cfname_, column.name, "",
                                           column.index_mode)) {
                    DB_LOG(ERROR, it->cfname_ << ": CreateIndex FAILED for "
                           << column.name);
                    return false;
                }
            }
        }
    }

    if (!dbif_->Db_SetTablespace(tablespace_)) {
        DB_LOG(ERROR, "Set KEYSPACE: " << tablespace_ << " FAILED");
        return false;
    }
    GenDb::ColList col_list;
    std::string cfname = g_viz_constants.SYSTEM_OBJECT_TABLE;
    GenDb::DbDataValueVec key;
    key.push_back(g_viz_constants.SYSTEM_OBJECT_ANALYTICS);

    bool init_done = false;
    if (dbif_->Db_GetRow(&col_list, cfname, key,
        GenDb::DbConsistency::LOCAL_ONE)) {
        for (GenDb::NewColVec::iterator it = col_list.columns_.begin();
                it != col_list.columns_.end(); it++) {
            std::string col_name;
            try {
                col_name = boost::get<std::string>(it->name->at(0));
            } catch (boost::bad_get& ex) {
                DB_LOG(ERROR, cfname << ": Column Name Get FAILED");
            }

            if (col_name == g_viz_constants.SYSTEM_OBJECT_START_TIME) {
                init_done = true;
            }
        }
    }

    if (!init_done) {
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.SYSTEM_OBJECT_TABLE;
        // Rowkey
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(1);
        rowkey.push_back(g_viz_constants.SYSTEM_OBJECT_ANALYTICS);
        // Columns
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(4);

        uint64_t current_tm = UTCTimestampUsec();

        GenDb::NewCol *col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_START_TIME, current_tm, 0));
        columns.push_back(col);

        GenDb::NewCol *flow_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_FLOW_START_TIME, current_tm, 0));
        columns.push_back(flow_col);

        GenDb::NewCol *msg_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_MSG_START_TIME, current_tm, 0));
        columns.push_back(msg_col);

        GenDb::NewCol *stat_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_STAT_START_TIME, current_tm, 0));
        columns.push_back(stat_col);

        if (!dbif_->Db_AddColumnSync(col_list,
            GenDb::DbConsistency::LOCAL_ONE)) {
            DB_LOG(ERROR, g_viz_constants.SYSTEM_OBJECT_TABLE <<
                ": Start Time Column Add FAILED");
            return false;
        }
    }

    /*
     * add ttls to cassandra to be retrieved by other daemons
     */
    {
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.SYSTEM_OBJECT_TABLE;
        // Rowkey
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(1);
        rowkey.push_back(g_viz_constants.SYSTEM_OBJECT_ANALYTICS);
        // Columns
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(4);

        GenDb::NewCol *col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_FLOW_DATA_TTL, (uint64_t)DbHandler::GetTtlInHourFromMap(ttl_map_, TtlType::FLOWDATA_TTL), 0));
        columns.push_back(col);

        GenDb::NewCol *flow_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_STATS_DATA_TTL, (uint64_t)DbHandler::GetTtlInHourFromMap(ttl_map_, TtlType::STATSDATA_TTL), 0));
        columns.push_back(flow_col);

        GenDb::NewCol *msg_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_CONFIG_AUDIT_TTL, (uint64_t)DbHandler::GetTtlInHourFromMap(ttl_map_, TtlType::CONFIGAUDIT_TTL), 0));
        columns.push_back(msg_col);

        GenDb::NewCol *stat_col(new GenDb::NewCol(
            g_viz_constants.SYSTEM_OBJECT_GLOBAL_DATA_TTL, (uint64_t)DbHandler::GetTtlInHourFromMap(ttl_map_, TtlType::GLOBAL_TTL), 0));
        columns.push_back(stat_col);

        if (!dbif_->Db_AddColumnSync(col_list,
            GenDb::DbConsistency::LOCAL_ONE)) {
            DB_LOG(ERROR, g_viz_constants.SYSTEM_OBJECT_TABLE <<
                ": TTL Column Add FAILED");
            return false;
        }
    }

    return true;
}

void DbHandler::UnInit() {
    dbif_->Db_Uninit();
    dbif_->Db_SetInitDone(false);
}

bool DbHandler::Init(bool initial) {
    SetDropLevel(0, SandeshLevel::INVALID, NULL);
    if (initial) {
        return Initialize();
    } else {
        return Setup();
    }
}

bool DbHandler::Initialize() {
    DB_LOG(DEBUG, "Initializing..");

    /* init of vizd table structures */
    init_vizd_tables();

    if (!dbif_->Db_Init()) {
        DB_LOG(ERROR, "Connection to DB FAILED");
        return false;
    }

    if (!dbif_->Db_AddSetTablespace(tablespace_, "2")) {
        DB_LOG(ERROR, "Create/Set KEYSPACE: " << tablespace_ << " FAILED");
        return false;
    }

    if (!CreateTables()) {
        DB_LOG(ERROR, "CreateTables FAILED");
        return false;
    }

    dbif_->Db_SetInitDone(true);
    DB_LOG(DEBUG, "Initializing Done");

    return true;
}

bool DbHandler::Setup() {
    DB_LOG(DEBUG, "Setup..");
    if (!dbif_->Db_Init()) {
        DB_LOG(ERROR, "Connection to DB FAILED");
        return false;
    }
    if (!dbif_->Db_SetTablespace(tablespace_)) {
        DB_LOG(ERROR, "Set KEYSPACE: " << tablespace_ << " FAILED");
        return false;
    }   
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_tables.begin();
            it != vizd_tables.end(); it++) {
        if (!dbif_->Db_UseColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << 
                   ": Db_UseColumnfamily FAILED");
            return false;
        }
    }
    for (std::vector<GenDb::NewCf>::const_iterator it = vizd_stat_tables.begin();
            it != vizd_stat_tables.end(); it++) {
        if (!dbif_->Db_UseColumnfamily(*it)) {
            DB_LOG(ERROR, it->cfname_ << ": Db_UseColumnfamily FAILED");
            return false;
        }
    }
    dbif_->Db_SetInitDone(true);
    DB_LOG(DEBUG, "Setup Done");
    return true;
}

bool DbHandler::IsAllWritesDisabled() const {
    return disable_all_writes_;
}

bool DbHandler::IsStatisticsWritesDisabled() const {
    return disable_statistics_writes_;
}

bool DbHandler::IsMessagesWritesDisabled() const {
    return disable_messages_writes_;
}

void DbHandler::DisableAllWrites(bool disable) {
    disable_all_writes_ = disable;
}

void DbHandler::DisableStatisticsWrites(bool disable) {
    disable_statistics_writes_ = disable;
}

void DbHandler::DisableMessagesWrites(bool disable) {
    disable_messages_writes_ = disable;
}

void DbHandler::SetDbQueueWaterMarkInfo(Sandesh::QueueWaterMarkInfo &wm,
    boost::function<void (void)> defer_undefer_cb) {
    dbif_->Db_SetQueueWaterMark(boost::get<2>(wm),
        boost::get<0>(wm),
        boost::bind(&DbHandler::SetDropLevel, this, _1, boost::get<1>(wm),
        defer_undefer_cb));
}

void DbHandler::ResetDbQueueWaterMarkInfo() {
    dbif_->Db_ResetQueueWaterMarks();
}

void DbHandler::GetSandeshStats(std::string *drop_level,
    std::vector<SandeshStats> *vdropmstats) const {
    *drop_level = Sandesh::LevelToString(drop_level_);
    if (vdropmstats) {
        tbb::mutex::scoped_lock lock(smutex_);
        dropped_msg_stats_.Get(vdropmstats);
    }
}

bool DbHandler::GetStats(uint64_t *queue_count, uint64_t *enqueues) const {
    return dbif_->Db_GetQueueStats(queue_count, enqueues);
}

bool DbHandler::GetStats(std::vector<GenDb::DbTableInfo> *vdbti,
    GenDb::DbErrors *dbe, std::vector<GenDb::DbTableInfo> *vstats_dbti) {
    {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.GetDiffs(vstats_dbti);
    }
    return dbif_->Db_GetStats(vdbti, dbe);
}

bool DbHandler::GetCumulativeStats(std::vector<GenDb::DbTableInfo> *vdbti,
    GenDb::DbErrors *dbe, std::vector<GenDb::DbTableInfo> *vstats_dbti) const {
    {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.GetCumulative(vstats_dbti);
    }
    return dbif_->Db_GetCumulativeStats(vdbti, dbe);
}

bool DbHandler::GetCqlMetrics(cass::cql::Metrics *metrics) const {
    cass::cql::CqlIf *cql_if(dynamic_cast<cass::cql::CqlIf *>(dbif_.get()));
    if (cql_if == NULL) {
        return false;
    }
    return cql_if->Db_GetCqlMetrics(metrics);
}

bool DbHandler::GetCqlStats(cass::cql::DbStats *stats) const {
    cass::cql::CqlIf *cql_if(dynamic_cast<cass::cql::CqlIf *>(dbif_.get()));
    if (cql_if == NULL) {
        return false;
    }
    return cql_if->Db_GetCqlStats(stats);
}

bool DbHandler::InsertIntoDb(std::auto_ptr<GenDb::ColList> col_list,
    GenDb::DbConsistency::type dconsistency,
    GenDb::GenDbIf::DbAddColumnCb db_cb) {
    if (IsAllWritesDisabled()) {
        return true;
    }
    return dbif_->Db_AddColumn(col_list, dconsistency, db_cb);
}

bool DbHandler::AllowMessageTableInsert(const SandeshHeader &header) {
    return !IsMessagesWritesDisabled() && !IsAllWritesDisabled() &&
        (header.get_Type() != SandeshType::FLOW) &&
        (header.get_Type() != SandeshType::SESSION);
}

void DbHandler::MessageTableOnlyInsert(const VizMsg *vmsgp,
    const DbHandler::ObjectNamesVec &object_names,
    GenDb::GenDbIf::DbAddColumnCb db_cb) {
    const SandeshHeader &header(vmsgp->msg->GetHeader());
    const std::string &message_type(vmsgp->msg->GetMessageType());
    uint64_t timestamp;
    int ttl;
    if (message_type == "VncApiConfigLog") {
        ttl = GetTtl(TtlType::CONFIGAUDIT_TTL);
    } else {
        ttl = GetTtl(TtlType::GLOBAL_TTL);
    }
    std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
    col_list->cfname_ = g_viz_constants.COLLECTOR_GLOBAL_TABLE;
    timestamp = header.get_Timestamp();
    uint32_t T2(timestamp >> g_viz_constants.RowTimeInBits);
    uint32_t T1(timestamp & g_viz_constants.RowTimeInMask);

    // Rowkey
    GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
    rowkey.reserve(2);
    rowkey.push_back(T2);
    rowkey.push_back(gen_partition_no_());

    // Columns
    GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec());
    col_name->reserve(20);
    col_name->push_back(T1);
    col_name->push_back(vmsgp->unm);

    // Prepend T2: to secondary index columns
    col_name->push_back(PrependT2(T2, header.get_Source()));
    col_name->push_back(PrependT2(T2, message_type));
    col_name->push_back(PrependT2(T2, header.get_Module()));

    // if number of entries in object_names > MSG_TABLE_MAX_OBJECTS_PER_MSG
    // - print error
    // - increment counter + export on introspect - TODO
    // - let first 6 entries go through
    unsigned int count = 0;
    BOOST_FOREACH(const std::string &object_name, object_names) {
        count++;
        if (count > g_viz_constants.MSG_TABLE_MAX_OBJECTS_PER_MSG) {
            DB_LOG(ERROR, "Number of object_names in message > " <<
                   g_viz_constants.MSG_TABLE_MAX_OBJECTS_PER_MSG <<
                   ". Ignoring extra object_names");
            break;
        }
        col_name->push_back(PrependT2(T2, object_name));
    }
    // Set the value as BLANK for remaining entries if
    // object_names.size() < MSG_TABLE_MAX_OBJECTS_PER_MSG
    for (int i = object_names.size();
         i < g_viz_constants.MSG_TABLE_MAX_OBJECTS_PER_MSG; i++) {
        col_name->push_back(GenDb::DbDataValue());
    }
    if (header.__isset.IPAddress) {
        boost::system::error_code ec;
        IpAddress ipaddr(IpAddress::from_string(header.get_IPAddress(), ec));
        if (ec ) {
            LOG(ERROR, "MessageTable: INVALID IP address:"
                << header.get_IPAddress());
            col_name->push_back(GenDb::DbDataValue());
        } else {
            col_name->push_back(ipaddr);
        }
    } else {
        col_name->push_back(GenDb::DbDataValue());
    }
    if (header.__isset.Pid) {
        col_name->push_back((uint32_t)header.get_Pid());
    } else {
        col_name->push_back(GenDb::DbDataValue());
    }
    col_name->push_back(header.get_Category());
    col_name->push_back((uint32_t)header.get_Level());
    col_name->push_back(header.get_NodeType());
    col_name->push_back(header.get_InstanceId());
    col_name->push_back((uint32_t)header.get_SequenceNum());
    col_name->push_back((uint8_t)header.get_Type());
    GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1,
        vmsgp->msg->ExtractMessage()));
    GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
    GenDb::NewColVec& columns = col_list->columns_;
    columns.reserve(1);
    columns.push_back(col);
    if (!InsertIntoDb(col_list, GenDb::DbConsistency::LOCAL_ONE, db_cb)) {
        DB_LOG(ERROR, "Addition of message: " << message_type <<
                ", message UUID: " << vmsgp->unm << " COLUMN FAILED");
        return;
    }
}

void DbHandler::MessageTableInsert(const VizMsg *vmsgp,
    const DbHandler::ObjectNamesVec &object_names,
    GenDb::GenDbIf::DbAddColumnCb db_cb) {
    const SandeshHeader &header(vmsgp->msg->GetHeader());
    const std::string &message_type(vmsgp->msg->GetMessageType());

    if (!AllowMessageTableInsert(header))
        return;

    MessageTableOnlyInsert(vmsgp, object_names, db_cb);


    const SandeshType::type &stype(header.get_Type());

    /*
     * Insert the message types,module_id in the stat table
     * Construct the atttributes,attrib_tags beofore inserting
     * to the StatTableInsert
     */
    if ((stype == SandeshType::SYSLOG) || (stype == SandeshType::SYSTEM)) {
        //Insert only if sandesh type is a SYSTEM LOG or SYSLOG
        //Insert into the FieldNames stats table entries for Messagetype and Module ID
        int ttl = GetTtl(TtlType::GLOBAL_TTL);
        FieldNamesTableInsert(header.get_Timestamp(),
            g_viz_constants.MESSAGE_TABLE,
            ":Messagetype", message_type, ttl, db_cb);
        FieldNamesTableInsert(header.get_Timestamp(),
            g_viz_constants.MESSAGE_TABLE,
            ":ModuleId", header.get_Module(), ttl, db_cb);
        FieldNamesTableInsert(header.get_Timestamp(),
            g_viz_constants.MESSAGE_TABLE,
            ":Source", header.get_Source(), ttl, db_cb);
        if (!header.get_Category().empty()) {
            FieldNamesTableInsert(header.get_Timestamp(),
                g_viz_constants.MESSAGE_TABLE,
                ":Category", header.get_Category(), ttl, db_cb);
        }
    }
}

/*
 * This function takes field name and field value as arguments and inserts
 * into the FieldNames stats table
 */
void DbHandler::FieldNamesTableInsert(uint64_t timestamp,
    const std::string& table_prefix, 
    const std::string& field_name, const std::string& field_val, int ttl,
    GenDb::GenDbIf::DbAddColumnCb db_cb) {
    /*
     * Insert the message types in the stat table
     * Construct the atttributes,attrib_tags before inserting
     * to the StatTableInsert
     */
    uint32_t temp_u32 = timestamp >> g_viz_constants.RowTimeInBits;
    std::string table_name(table_prefix);
    table_name.append(field_name);

    /* Check if fieldname and value were already seen in this T2;
       2 caches are mainted one for  last T2 and T2-1.
       We only need to record them if they have NOT been seen yet */
    bool record = false;
    std::string fc_entry(table_name);
    fc_entry.append(":");
    fc_entry.append(field_val);
    {
        tbb::mutex::scoped_lock lock(fmutex_);
        record = CanRecordDataForT2(temp_u32, fc_entry);
    }

    if (!record) return;

    DbHandler::TagMap tmap;
    DbHandler::AttribMap amap;
    DbHandler::Var pv;
    DbHandler::AttribMap attribs;
    pv = table_name;
    tmap.insert(make_pair("name", make_pair(pv, amap)));
    attribs.insert(make_pair(string("name"), pv));
    string sattrname("fields.value");
    pv = string(field_val);
    attribs.insert(make_pair(sattrname,pv));

    //pv = string(header.get_Source());
    // Put the name of the collector, not the message source.
    // Using the message source will make queries slower
    pv = string(col_name_);
    tmap.insert(make_pair("Source",make_pair(pv,amap))); 
    attribs.insert(make_pair(string("Source"),pv));

    StatTableInsertTtl(timestamp, "FieldNames","fields", tmap, attribs, ttl,
        db_cb);
}

/*
 * This function checks if the data can be recorded or not
 * for the given t2. If t2 corresponding to the data is
 * older than field_cache_old_t2_ and field_cache_t2_
 * it is ignored
 */
bool DbHandler::CanRecordDataForT2(uint32_t temp_u32, std::string fc_entry) {
    bool record = false;

    uint32_t cacheindex = temp_u32 >> g_viz_constants.CacheTimeInAdditionalBits;
    if (cacheindex > field_cache_index_) {
            field_cache_index_ = cacheindex;
            field_cache_set_.clear();
            field_cache_set_.insert(fc_entry);
            record = true;
    } else if (cacheindex == field_cache_index_) {
        if (field_cache_set_.find(fc_entry) ==
            field_cache_set_.end()) {
            field_cache_set_.insert(fc_entry);
            record = true;
        }
    }
    return record;
}

void DbHandler::GetRuleMap(RuleMap& rulemap) {
}

/*
 * insert an entry into an ObjectTrace table
 * key is T2
 * column is
 *  name: <key>:T1 (value in timestamp)
 *  value: uuid (of the corresponding global message)
 */
void DbHandler::ObjectTableInsert(const std::string &table, const std::string &objectkey_str,
        uint64_t &timestamp, const boost::uuids::uuid& unm, const VizMsg *vmsgp,
        GenDb::GenDbIf::DbAddColumnCb db_cb) {
    if (IsMessagesWritesDisabled() || IsAllWritesDisabled()) {
        return;
    }
    uint32_t T2(timestamp >> g_viz_constants.RowTimeInBits);
    uint32_t T1(timestamp & g_viz_constants.RowTimeInMask);
    const std::string &message_type(vmsgp->msg->GetMessageType());
    int ttl;
    if (message_type == "VncApiConfigLog") {
        ttl = GetTtl(TtlType::CONFIGAUDIT_TTL);
    } else {
        ttl = GetTtl(TtlType::GLOBAL_TTL);
    }

    {
        std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
        col_list->cfname_ = g_viz_constants.OBJECT_VALUE_TABLE;
        GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
        rowkey.reserve(2);
        rowkey.push_back(T2);
        rowkey.push_back(table);
        GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec(1, T1));
        GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, objectkey_str));
        GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
        GenDb::NewColVec& columns = col_list->columns_;
        columns.reserve(1);
        columns.push_back(col);
        if (!InsertIntoDb(col_list, GenDb::DbConsistency::LOCAL_ONE, db_cb)) {
            DB_LOG(ERROR, "Addition of " << objectkey_str <<
                    ", message UUID " << unm << " " << table << " into table "
                    << g_viz_constants.OBJECT_VALUE_TABLE << " FAILED");
            return;
        }

        /*
         * Inserting into the stat table
         */
        const SandeshHeader &header(vmsgp->msg->GetHeader());
        const std::string &message_type(vmsgp->msg->GetMessageType());
        //Insert into the FieldNames stats table entries for Messagetype and Module ID
        FieldNamesTableInsert(timestamp,
                table, ":ObjectId", objectkey_str, ttl, db_cb);
        FieldNamesTableInsert(timestamp,
                table, ":Messagetype", message_type, ttl, db_cb);
        FieldNamesTableInsert(timestamp,
                table, ":ModuleId", header.get_Module(), ttl, db_cb);
        FieldNamesTableInsert(timestamp,
                table, ":Source", header.get_Source(), ttl, db_cb);

        FieldNamesTableInsert(timestamp,
                "OBJECT:", table, table, ttl, db_cb);
    }
}

bool DbHandler::StatTableWrite(uint32_t t2, const std::string& statName,
        const std::string& statAttr, const std::string& source, const std::string& name,
        const std::string& key, const std::string& proxy,
        const std::vector<std::vector<std::string> >& tags,
        uint32_t t1, const boost::uuids::uuid& unm,
        const std::string& jsonline, int ttl,
        GenDb::GenDbIf::DbAddColumnCb db_cb) {

    uint8_t part = 0;
    std::auto_ptr<GenDb::ColList> col_list(new GenDb::ColList);
    col_list->cfname_ = g_viz_constants.STATS_TABLE;

    GenDb::DbDataValueVec& rowkey = col_list->rowkey_;
    rowkey.reserve(4);
    rowkey.push_back(t2);
    rowkey.push_back(part);
    rowkey.push_back(statName);
    rowkey.push_back(statAttr);

    GenDb::DbDataValueVec *col_name(new GenDb::DbDataValueVec);
    col_name->reserve(6);
    col_name->push_back(name);
    col_name->push_back(t1);
    col_name->push_back(unm);
    col_name->push_back(PrependT2(t2, source));
    col_name->push_back(PrependT2(t2, key));
    col_name->push_back(PrependT2(t2, proxy));
    col_name->push_back(PrependT2(t2, boost::algorithm::join(tags[0], ";")));
    col_name->push_back(PrependT2(t2, boost::algorithm::join(tags[1], ";")));
    col_name->push_back(PrependT2(t2, boost::algorithm::join(tags[2], ";")));
    col_name->push_back(PrependT2(t2, boost::algorithm::join(tags[3], ";")));

    GenDb::DbDataValueVec *col_value(new GenDb::DbDataValueVec(1, jsonline));
    GenDb::NewCol *col(new GenDb::NewCol(col_name, col_value, ttl));
    GenDb::NewColVec& columns = col_list->columns_;
    columns.push_back(col);

    if (!InsertIntoDb(col_list, GenDb::DbConsistency::LOCAL_ONE, db_cb)) {
        DB_LOG(ERROR, "Addition of " << statName <<
                ", " << statAttr << " into table " <<
                g_viz_constants.STATS_TABLE <<" FAILED");
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Update(statName + ":" + statAttr, true, true, false, 1);
        return false;
    } else {
        tbb::mutex::scoped_lock lock(smutex_);
        stable_stats_.Update(statName + ":" + statAttr, true, false, false, 1);
        return true;
    }
}

void
DbHandler::StatTableInsert(uint64_t ts, 
        const std::string& statName,
        const std::string& statAttr,
        const TagMap & attribs_tag,
        const AttribMap & attribs,
        GenDb::GenDbIf::DbAddColumnCb db_cb) {
    if (IsAllWritesDisabled() || IsStatisticsWritesDisabled()) {
        return;
    }
    int ttl = GetTtl(TtlType::STATSDATA_TTL);
    StatTableInsertTtl(ts, statName, statAttr, attribs_tag, attribs, ttl,
        db_cb);
}

static inline unsigned int djb_hash (const char *str, size_t len) {
    unsigned int hash = 5381;
    for (size_t i = 0 ; i < len ; i++)
        hash = ((hash << 5) + hash) + str[i];
    return hash;
}

typedef std::pair<std::string, std::string> MapElem;

// This function writes Stats samples to the DB.
void
DbHandler::StatTableInsertTtl(uint64_t ts, 
        const std::string& statName,
        const std::string& statAttr,
        const TagMap & attribs_tag,
        const AttribMap & attribs, int ttl,
        GenDb::GenDbIf::DbAddColumnCb db_cb) {

    uint64_t temp_u64 = ts;
    uint32_t temp_u32 = temp_u64 >> g_viz_constants.RowTimeInBits;
    boost::uuids::uuid unm;
    if (statName.compare("FieldNames") != 0) {
         unm = umn_gen_();
    }

    // This is very primitive JSON encoding.
    // Should replace with rapidJson at some point.

    // Encoding of all attribs

    contrail_rapidjson::Document dd;
    dd.SetObject();

    AttribMap attribs_buf;
    for (AttribMap::const_iterator it = attribs.begin();
            it != attribs.end(); it++) {
        switch (it->second.type) {
            case STRING: {
                    contrail_rapidjson::Value val(contrail_rapidjson::kStringType);
                    std::string nm = it->first + std::string("|s");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetString(it->second.str.c_str(), dd.GetAllocator());
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(rt.first->first.c_str(),
                                 dd.GetAllocator()), val, dd.GetAllocator());
                    string field_name = it->first;
                     if (field_name.compare("fields.value") == 0) {
                         if (statName.compare("FieldNames") == 0) {
                             //Make uuid a fn of the field.values
                             boost::uuids::name_generator gen(DbHandler::seed_uuid);
                             unm = gen(it->second.str.c_str());
                         }
                     }
                }
                break;
            case UINT64: {
                    contrail_rapidjson::Value val(contrail_rapidjson::kNumberType);
                    std::string nm = it->first + std::string("|n");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetUint64(it->second.num);
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(rt.first->first.c_str(),
                                 dd.GetAllocator()), val, dd.GetAllocator());
                }
                break;
            case DOUBLE: {
                    contrail_rapidjson::Value val(contrail_rapidjson::kNumberType);
                    std::string nm = it->first + std::string("|d");
                    pair<AttribMap::iterator,bool> rt = 
                        attribs_buf.insert(make_pair(nm, it->second));
                    val.SetDouble(it->second.dbl);
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(rt.first->first.c_str(),
                                 dd.GetAllocator()), val, dd.GetAllocator());
                }
                break;
            case LIST: {
                    contrail_rapidjson::Value val_array(contrail_rapidjson::kArrayType);
                    std::string nm = it->first + std::string("|a");
                    BOOST_FOREACH(const std::string& elem, it->second.vec) {
                        contrail_rapidjson::Value val(contrail_rapidjson::kStringType);
                        val.SetString(elem.c_str(), dd.GetAllocator());
                        val_array.PushBack(val, dd.GetAllocator());
                    }
                    pair<AttribMap::iterator,bool> rt =
                        attribs_buf.insert(make_pair(nm, it->second));
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(rt.first->first.c_str(),
                                 dd.GetAllocator()), val_array, dd.GetAllocator());

                }
                break;
            case MAP: {
                    contrail_rapidjson::Value val_obj(contrail_rapidjson::kObjectType);
                    std::string nm = it->first + std::string("|m");
                    BOOST_FOREACH(const MapElem& pair, it->second.map) {
                        contrail_rapidjson::Value val(contrail_rapidjson::kStringType);
                        val.SetString(pair.second.c_str(), dd.GetAllocator());
                        contrail_rapidjson::Value val_key;
                        val_obj.AddMember(val_key.SetString(pair.first.c_str(),
                            dd.GetAllocator()), val, dd.GetAllocator());
                    }
                    pair<AttribMap::iterator,bool> rt =
                        attribs_buf.insert(make_pair(nm, it->second));
                    contrail_rapidjson::Value vk;
                    dd.AddMember(vk.SetString(rt.first->first.c_str(),
                                 dd.GetAllocator()), val_obj, dd.GetAllocator());

                }
                break;
            default:
                continue;
        }
    }

    contrail_rapidjson::StringBuffer sb;
    contrail_rapidjson::Writer<contrail_rapidjson::StringBuffer> writer(sb);
    dd.Accept(writer);
    string jsonline(sb.GetString());

    uint32_t t1;
    t1 = (uint32_t)(temp_u64& g_viz_constants.RowTimeInMask);

    if ( statName.compare("FieldNames") != 0) {
        std::string tablename(std::string("StatTable.") + statName + "." + statAttr);
        FieldNamesTableInsert(ts,
                    "STAT:", tablename, tablename, ttl, db_cb);
    }

    std::vector<std::vector<std::string> > tags(4);
    std::string name, source, key, proxy;
    for (TagMap::const_iterator it = attribs_tag.begin();
            it != attribs_tag.end(); it++) {

        pair<string,DbHandler::Var> ptag;
        ptag.first = it->first;
        ptag.second = it->second.first;

        /* Record in the fieldNames table if we have a string tag,
           and if we are not recording a fieldNames stats entry itself */
        if ((ptag.second.type == DbHandler::STRING) &&
                (statName.compare("FieldNames") != 0)) {
            FieldNamesTableInsert(ts, std::string("StatTable.") +
                    statName + "." + statAttr,
                    std::string(":") + ptag.first, ptag.second.str, ttl,
                    db_cb);
        }

        if (ptag.first == g_viz_constants.STATS_NAME_FIELD) {
            name = ptag.second.str;
        } else if (ptag.first == g_viz_constants.STATS_SOURCE_FIELD) {
            source = ptag.second.str;
        } else if (boost::algorithm::ends_with(ptag.first, g_viz_constants.STATS_KEY_FIELD)) {
            key = ptag.second.str;
        } else if (boost::algorithm::ends_with(ptag.first, g_viz_constants.STATS_PROXY_FIELD)) {
            proxy = ptag.second.str;
        } else {
            switch (ptag.second.type) {
                case STRING:
                case UINT64:
                case DOUBLE: {
                        std::ostringstream tag_oss;
                        tag_oss << ptag.first << "=" << ptag.second;
                        size_t idx = djb_hash(ptag.first.c_str(), ptag.first.length())
                            % g_viz_constants.NUM_STATS_TAGS_FIELD;
                        tags[idx].push_back(tag_oss.str());
                    }
                    break;
                case LIST: {
                        BOOST_FOREACH(const std::string& elem, ptag.second.vec) {
                            std::ostringstream tag_oss;
                            tag_oss << ptag.first << "=" << elem;
                            size_t idx = djb_hash(ptag.first.c_str(), ptag.first.length())
                                % g_viz_constants.NUM_STATS_TAGS_FIELD;
                            tags[idx].push_back(tag_oss.str());
                        }
                    }
                    break;
                case MAP: {
                        BOOST_FOREACH(const MapElem& pair, ptag.second.map) {
                            std::ostringstream tag_oss;
                            std::string nm = ptag.first + "." + pair.first;
                            tag_oss << nm << "=" << pair.second;
                            size_t idx = djb_hash(nm.c_str(), nm.length())
                                % g_viz_constants.NUM_STATS_TAGS_FIELD;
                            tags[idx].push_back(tag_oss.str());
                        }
                    }
                    break;
                default: {
                    continue;
                }
            }
        }

        if (!it->second.second.empty()) {
            for (AttribMap::const_iterator jt = it->second.second.begin();
                    jt != it->second.second.end(); jt++) { 
                if (jt->first == g_viz_constants.STATS_NAME_FIELD) {
                    name = jt->second.str;
                } else if (jt->first == g_viz_constants.STATS_SOURCE_FIELD) {
                    source = jt->second.str;
                } else if (boost::algorithm::ends_with(jt->first, g_viz_constants.STATS_KEY_FIELD)) {
                    key = jt->second.str;
                } else if (boost::algorithm::ends_with(jt->first, g_viz_constants.STATS_PROXY_FIELD)) {
                    proxy = jt->second.str;
                } else {
                    std::ostringstream tag_oss;
                    tag_oss << jt->first << "=" << jt->second;
                    size_t idx = djb_hash(jt->first.c_str(), jt->first.length())
                        % g_viz_constants.NUM_STATS_TAGS_FIELD;
                    tags[idx].push_back(tag_oss.str());
                    switch (jt->second.type) {
                    case STRING:
                    case UINT64:
                    case DOUBLE: {
                            std::ostringstream tag_oss;
                            tag_oss << jt->first << "=" << jt->second;
                            size_t idx = djb_hash(jt->first.c_str(), jt->first.length())
                                % g_viz_constants.NUM_STATS_TAGS_FIELD;
                            tags[idx].push_back(tag_oss.str());
                        }
                        break;
                    case LIST: {
                            BOOST_FOREACH(const std::string& elem, jt->second.vec) {
                                std::ostringstream tag_oss;
                                tag_oss << jt->first << "=" << elem;
                                size_t idx = djb_hash(jt->first.c_str(), jt->first.length())
                                    % g_viz_constants.NUM_STATS_TAGS_FIELD;
                                tags[idx].push_back(tag_oss.str());
                            }
                        }
                        break;
                    case MAP: {
                            BOOST_FOREACH(const MapElem& pair, jt->second.map) {
                                std::ostringstream tag_oss;
                                std::string nm = jt->first + "." + pair.first;
                                tag_oss << nm << "=" << pair.second;
                                size_t idx = djb_hash(nm.c_str(), nm.length())
                                    % g_viz_constants.NUM_STATS_TAGS_FIELD;
                                tags[idx].push_back(tag_oss.str());
                            }
                        }
                        break;
                    default: {
                        continue;
                        }
                    }
                }
            }
        }
    }
    StatTableWrite(temp_u32, statName, statAttr, source, name, key, proxy, tags, t1,
                        unm, jsonline, ttl, db_cb);
}

boost::uuids::uuid DbHandler::seed_uuid = StringToUuid(std::string("ffffffff-ffff-ffff-ffff-ffffffffffff"));

SessionValueArray default_col_values = boost::assign::list_of
    (GenDb::DbDataValue((uint32_t)0))
    (GenDb::DbDataValue((uint8_t)0))
    (GenDb::DbDataValue((uint8_t)0))
    (GenDb::DbDataValue((uint8_t)0))
    (GenDb::DbDataValue((uint16_t)0))
    (GenDb::DbDataValue((uint16_t)0))
    (GenDb::DbDataValue((uint32_t)0))
    (GenDb::DbDataValue(boost::uuids::nil_uuid()))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue("__UNKNOWN__"))
    (GenDb::DbDataValue(""))
    (GenDb::DbDataValue(IpAddress()))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue((uint64_t)0))
    (GenDb::DbDataValue(""));

static bool PopulateSessionTable(uint32_t t2, SessionValueArray& svalues,
    DbInsertCb db_insert_cb, TtlMap& ttl_map) {

    std::auto_ptr<GenDb::ColList> colList(new GenDb::ColList);
    // RowKey
    colList->rowkey_.reserve(3);
    colList->rowkey_.push_back(svalues[SessionRecordFields::SESSION_T2]);
    colList->rowkey_.push_back(svalues[SessionRecordFields::SESSION_PARTITION_NO]);
    colList->rowkey_.push_back(svalues[SessionRecordFields::SESSION_IS_SI]);
    colList->rowkey_.push_back(svalues[
        SessionRecordFields::SESSION_IS_CLIENT_SESSION]);

    // Column Names
    GenDb::DbDataValueVec* cnames(new GenDb::DbDataValueVec);
    for (int sfield = g_viz_constants.SESSION_MIN;
            sfield != g_viz_constants.SESSION_MAX - 1; sfield++) {
        if (svalues[sfield].which() == GenDb::DB_VALUE_BLANK) {
            if (sfield >= g_viz_constants.SESSION_INDEX_MIN &&
                sfield <= g_viz_constants.SESSION_INDEX_MAX) {
                cnames->push_back(integerToString(t2) + ":" + g_viz_constants.UNKNOWN);
            } else {
                cnames->push_back(default_col_values[sfield]);
            }
        }
        else {
            cnames->push_back(svalues[sfield]);
        }
    }

    // Column Values
    GenDb::DbDataValueVec* cvalue(new GenDb::DbDataValueVec);
    cvalue->reserve(1);
    cvalue->push_back(svalues[SessionRecordFields::SESSION_MAP]);

    int ttl = DbHandler::GetTtlFromMap(ttl_map, TtlType::FLOWDATA_TTL);

    colList->cfname_ = g_viz_constants.SESSION_TABLE;
    colList->columns_.push_back(new GenDb::NewCol(cnames, cvalue, ttl));
    if (!db_insert_cb(colList)) {
            LOG(ERROR, "Populating SessionTable FAILED");
    }

    return true;
}

void JsonifySessionMap(const pugi::xml_node& root, std::string *json_string) {

    contrail_rapidjson::Document session_map;
    session_map.SetObject();
    contrail_rapidjson::Document::AllocatorType& allocator =
        session_map.GetAllocator();

    for (pugi::xml_node ip_port = root.first_child(); ip_port; ip_port =
        ip_port.next_sibling().next_sibling()) {
        std::ostringstream ip_port_ss;
        ip_port_ss << ip_port.child(g_flow_constants.PORT.c_str()).child_value() << ":"
            << ip_port.child(g_flow_constants.IP.c_str()).child_value();
        contrail_rapidjson::Value session_val(contrail_rapidjson::kObjectType);
        pugi::xml_node session(ip_port.next_sibling());
        for (pugi::xml_node field = session.first_child(); field;
            field = field.next_sibling()) {
            contrail_rapidjson::Value fk(contrail_rapidjson::kStringType);
            std::string fname(field.name());
            uint64_t val;
            if (fname == "forward_flow_info" || fname == "reverse_flow_info") {
                contrail_rapidjson::Value flow_info(contrail_rapidjson::kObjectType);
                for (pugi::xml_node finfo = field.child("SessionFlowInfo").first_child();
                    finfo; finfo = finfo.next_sibling()) {
                    std::string name(finfo.name());
                    std::string value(finfo.child_value());
                    std::map<std::string, bool>::const_iterator it =
                        g_flow_constants.SessionFlowInfoField2Type.find(name);
                    assert(it != g_flow_constants.SessionFlowInfoField2Type.end());
                    if (it->second) {
                        stringToInteger(value, val);
                        contrail_rapidjson::Value fv(contrail_rapidjson::kNumberType);
                        flow_info.AddMember(fk.SetString(name.c_str(), allocator),
                            fv.SetUint64(val), allocator);
                    } else {
                        contrail_rapidjson::Value fv(contrail_rapidjson::kStringType);
                        flow_info.AddMember(fk.SetString(name.c_str(), allocator),
                            fv.SetString(value.c_str(), allocator), allocator);
                    }
                }
                session_val.AddMember(fk.SetString(fname.c_str(), allocator),
                    flow_info, allocator);
            } else {
                std::string fvalue(field.child_value());
                if (stringToInteger(fvalue, val)) {
                    contrail_rapidjson::Value fv(contrail_rapidjson::kNumberType);
                    session_val.AddMember(fk.SetString(fname.c_str(), allocator),
                        fv.SetUint64(val), allocator);
                } else {
                    contrail_rapidjson::Value fv(contrail_rapidjson::kStringType);
                    session_val.AddMember(fk.SetString(fname.c_str(), allocator),
                        fv.SetString(fvalue.c_str(), allocator), allocator);
                }
            }
        }
        contrail_rapidjson::Value vk(contrail_rapidjson::kStringType);
        session_map.AddMember(vk.SetString(ip_port_ss.str().c_str(),
            allocator), session_val, allocator);
    }

    contrail_rapidjson::StringBuffer sb;
    contrail_rapidjson::Writer<contrail_rapidjson::StringBuffer> writer(sb);
    session_map.Accept(writer);
    *json_string = sb.GetString();
}

/*
 * process the session sample and insert into the appropriate table
 */
bool DbHandler::SessionSampleAdd(const pugi::xml_node& session_sample,
                                 const SandeshHeader& header,
                                 GenDb::GenDbIf::DbAddColumnCb db_cb) {
    SessionValueArray session_entry_values;
    pugi::xml_node &mnode = const_cast<pugi::xml_node &>(session_sample);

    // Set T1 and T2 from timestamp
    uint64_t timestamp(header.get_Timestamp());
    uint32_t T2(timestamp >> g_viz_constants.RowTimeInBits);
    uint32_t T1(timestamp & g_viz_constants.RowTimeInMask);
    session_entry_values[SessionRecordFields::SESSION_T2] = T2;
    session_entry_values[SessionRecordFields::SESSION_T1] = T1;
    // vrouter
    session_entry_values[SessionRecordFields::SESSION_VROUTER] = header.get_Source();

    pugi::xml_node session_agg_info_node;
    // Populate session_entry_values from message
    for (pugi::xml_node sfield = mnode.first_child(); sfield;
            sfield = sfield.next_sibling()) {

        std::string col_type(sfield.attribute("type").value());
        std::string col_name(sfield.name());
        SessionTypeMap::const_iterator it = session_msg2type_map.find(col_name);
        if (it != session_msg2type_map.end()) {
            const SessionTypeInfo &stinfo(it->second);

            if (col_type == "set") {
                pugi::xml_node set = sfield.child("set");
                std::ostringstream set_value;
                set_value << T2 << ":";
                int i = 0;
                for (pugi::xml_node set_elem = set.first_child(); set_elem;
                        set_elem = set_elem.next_sibling()) {
                    if (i) {
                        set_value << ";";
                    }
                    std::string val = set_elem.child_value();
                    TXMLProtocol::unescapeXMLControlChars(val);
                    set_value << val;
                    i++;
                }
                session_entry_values[stinfo.get<0>()] = set_value.str();
                continue;
            }

            switch(stinfo.get<1>()) {
            case GenDb::DbDataType::Unsigned8Type:
                {
                    uint8_t val;
                    stringToInteger(sfield.child_value(), val);
                    session_entry_values[stinfo.get<0>()] =
                        static_cast<uint8_t>(val);
                    break;
                }
            case GenDb::DbDataType::Unsigned16Type:
                {
                    uint16_t val;
                    stringToInteger(sfield.child_value(), val);
                    session_entry_values[stinfo.get<0>()] =
                        static_cast<uint16_t>(val);
                    break;
                }
            case GenDb::DbDataType::Unsigned32Type:
                {
                    uint32_t val;
                    stringToInteger(sfield.child_value(), val);
                    session_entry_values[stinfo.get<0>()] =
                        static_cast<uint32_t>(val);
                    break;
                }
            case GenDb::DbDataType::Unsigned64Type:
                {
                    uint64_t val;
                    stringToInteger(sfield.child_value(), val);
                    session_entry_values[stinfo.get<0>()] =
                        static_cast<uint64_t>(val);
                    break;
                }
            case GenDb::DbDataType::LexicalUUIDType:
            case GenDb::DbDataType::TimeUUIDType:
                {
                    std::stringstream ss;
                    ss << sfield.child_value();
                    boost::uuids::uuid u;
                    if (!ss.str().empty()) {
                        ss >> u;
                        if (ss.fail()) {
                            LOG(ERROR, "SessionTable: " << col_name << ": (" <<
                                sfield.child_value() << ") INVALID");
                        }
                    }
                    session_entry_values[stinfo.get<0>()] = u;
                    break;
                }
            case GenDb::DbDataType::AsciiType:
            case GenDb::DbDataType::UTF8Type:
                {
                    std::string val = sfield.child_value();
                    TXMLProtocol::unescapeXMLControlChars(val);
                    switch(stinfo.get<0>()) {
                    case SessionRecordFields::SESSION_DEPLOYMENT:
                    case SessionRecordFields::SESSION_TIER:
                    case SessionRecordFields::SESSION_APPLICATION:
                    case SessionRecordFields::SESSION_SITE:
                    case SessionRecordFields::SESSION_REMOTE_DEPLOYMENT:
                    case SessionRecordFields::SESSION_REMOTE_TIER:
                    case SessionRecordFields::SESSION_REMOTE_APPLICATION:
                    case SessionRecordFields::SESSION_REMOTE_SITE:
                    case SessionRecordFields::SESSION_REMOTE_PREFIX:
                    case SessionRecordFields::SESSION_SECURITY_POLICY_RULE:
                    case SessionRecordFields::SESSION_VMI:
                    case SessionRecordFields::SESSION_VN:
                    case SessionRecordFields::SESSION_REMOTE_VN:
                        {
                            std::ostringstream v;
                            v << T2 << ":" << val;
                            session_entry_values[stinfo.get<0>()] = v.str();
                            break;
                        }
                    default:
                        {
                            session_entry_values[stinfo.get<0>()] = val;
                            break;
                        }
                    }
                    break;
                }
            case GenDb::DbDataType::InetType:
                {
                    boost::system::error_code ec;
                    IpAddress ipaddr(IpAddress::from_string(
                                     sfield.child_value(), ec));
                    if (ec) {
                        LOG(ERROR, "SessionRecordTable: " << col_name << ": ("
                            << sfield.child_value() << ") INVALID");
                    }
                    session_entry_values[stinfo.get<0>()] = ipaddr;
                    break;
                }
            default:
                {
                    VIZD_ASSERT(0);
                    break;
                }
            }
        } else if (col_type == "map" && col_name == "sess_agg_info") {
            session_agg_info_node = sfield.child("map");
            continue;
        }
    }

    for (pugi::xml_node ip_port_proto = session_agg_info_node.first_child();
        ip_port_proto; ip_port_proto = ip_port_proto.next_sibling().next_sibling()) {
        uint16_t val;
        stringToInteger(ip_port_proto.child(g_flow_constants.SERVICE_PORT.c_str()).child_value(), val);
        session_entry_values[SessionRecordFields::SESSION_SPORT] = val;
        stringToInteger(ip_port_proto.child(g_flow_constants.PROTOCOL.c_str()).child_value(), val);
        session_entry_values[SessionRecordFields::SESSION_PROTOCOL] = val;
        session_entry_values[SessionRecordFields::SESSION_UUID] = umn_gen_();
        // Partition No
        uint8_t partition_no = gen_partition_no_();
        session_entry_values[SessionRecordFields::SESSION_PARTITION_NO] = partition_no;
        std::ostringstream oss;
        oss << T2 << ":" << ip_port_proto.child(g_flow_constants.LOCAL_IP.c_str()).child_value();
        session_entry_values[SessionRecordFields::SESSION_IP] = oss.str();
        pugi::xml_node sess_agg_info = ip_port_proto.next_sibling();
        for (pugi::xml_node agg_info = sess_agg_info.first_child();
            agg_info; agg_info = agg_info.next_sibling()) {
            if (strcmp(agg_info.attribute("type").value(), "map") == 0) {
                int16_t samples;
                stringToInteger(agg_info.child("map").attribute("size").value(), samples);
                session_table_db_stats_.num_samples += samples;
                std::string session_map;
                JsonifySessionMap(agg_info.child("map"), &session_map);
                session_table_db_stats_.curr_json_size += session_map.size(); 
                session_entry_values[SessionRecordFields::SESSION_MAP]
                    = session_map;
                continue;
            }
            std::string field_name(agg_info.name());
            SessionTypeMap::const_iterator it =
                session_msg2type_map.find(field_name);
            if (it != session_msg2type_map.end()) {
                const SessionTypeInfo &stinfo(it->second);
                uint64_t val;
                stringToInteger(agg_info.child_value(), val);
                session_entry_values[stinfo.get<0>()]
                    = static_cast<uint64_t>(val);
            }
        }
        DbInsertCb db_insert_cb =
            boost::bind(&DbHandler::InsertIntoDb, this, _1,
            GenDb::DbConsistency::LOCAL_ONE, db_cb);
        if (!PopulateSessionTable(T2, session_entry_values,
            db_insert_cb, ttl_map_)) {
                DB_LOG(ERROR, "Populating SessionRecordTable FAILED");
        }
        session_table_db_stats_.num_writes++;
    }

    int ttl = DbHandler::GetTtlFromMap(ttl_map_, TtlType::FLOWDATA_TTL);
    // insert into FieldNames table
    FieldNamesTableInsert(timestamp, g_viz_constants.SESSION_TABLE, ":vrouter",
        boost::get<std::string>(session_entry_values[
            SessionRecordFields::SESSION_VROUTER]), ttl, db_cb);
    FieldNamesTableInsert(timestamp, g_viz_constants.SESSION_TABLE, ":vn",
        boost::get<std::string>(session_entry_values[
            SessionRecordFields::SESSION_VN]), ttl, db_cb);
    FieldNamesTableInsert(timestamp, g_viz_constants.SESSION_TABLE, ":remote_vn",
        boost::get<std::string>(session_entry_values[
            SessionRecordFields::SESSION_REMOTE_VN]), ttl, db_cb);

    session_table_db_stats_.num_messages++;
    return true;
}

/*
 * process the session sandesh message
 */

bool DbHandler::SessionTableInsert(const pugi::xml_node &parent,
    const SandeshHeader& header, GenDb::GenDbIf::DbAddColumnCb db_cb) {
    pugi::xml_node session_data(parent.child("session_data"));
    if (!session_data) {
        return true;
    }
    // Session sandesh message may contain a list of session samples or
    // a single session sample
    if (strcmp(session_data.attribute("type").value(), "list") == 0) {
        pugi::xml_node session_list = session_data.child("list");
        for (pugi::xml_node ssample = session_list.first_child(); ssample;
            ssample = ssample.next_sibling()) {
            SessionSampleAdd(ssample, header, db_cb);
        }
    } else {
        SessionSampleAdd(session_data.first_child(), header, db_cb);
    }
    return true;
}

bool DbHandler::GetSessionTableDbInfo(SessionTableDbInfo *session_table_info) {
    {
        tbb::mutex::scoped_lock lock(smutex_);
        if (session_table_db_stats_.num_messages == 0) {
            return true;
        }
        double writes_per_message = (double)session_table_db_stats_.num_writes /
                                        session_table_db_stats_.num_messages;
        double session_per_db_write = (double)session_table_db_stats_.num_samples /
                                        session_table_db_stats_.num_writes;
        double json_size_per_write = (double)session_table_db_stats_.curr_json_size /
                                        session_table_db_stats_.num_writes;
        session_table_info->set_writes_per_message(writes_per_message);
        session_table_info->set_sessions_per_db_record(session_per_db_write);
        session_table_info->set_json_size_per_write(json_size_per_write);
        session_table_info->set_num_messages(session_table_db_stats_.num_messages);
        session_table_db_stats_.num_writes = 0;
        session_table_db_stats_.num_samples = 0;
        session_table_db_stats_.num_messages = 0;
        session_table_db_stats_.curr_json_size = 0;
    }
    return true;
}

bool DbHandler::UnderlayFlowSampleInsert(const UFlowData& flow_data,
                                         uint64_t timestamp,
                                         GenDb::GenDbIf::DbAddColumnCb db_cb) {
    const std::vector<UFlowSample>& flow = flow_data.get_flow();
    for (std::vector<UFlowSample>::const_iterator it = flow.begin();
         it != flow.end(); ++it) {
        // Add all attributes
        DbHandler::AttribMap amap;
        DbHandler::Var name(flow_data.get_name());
        amap.insert(std::make_pair("name", name));
        DbHandler::Var pifindex = it->get_pifindex();
        amap.insert(std::make_pair("flow.pifindex", pifindex));
        DbHandler::Var sip = it->get_sip();
        amap.insert(std::make_pair("flow.sip", sip));
        DbHandler::Var dip = it->get_dip();
        amap.insert(std::make_pair("flow.dip", dip));
        DbHandler::Var sport = static_cast<uint64_t>(it->get_sport());
        amap.insert(std::make_pair("flow.sport", sport));
        DbHandler::Var dport = static_cast<uint64_t>(it->get_dport());
        amap.insert(std::make_pair("flow.dport", dport));
        DbHandler::Var protocol = static_cast<uint64_t>(it->get_protocol());
        amap.insert(std::make_pair("flow.protocol", protocol));
        DbHandler::Var ft = it->get_flowtype();
        amap.insert(std::make_pair("flow.flowtype", ft));
        
        DbHandler::TagMap tmap;
        // Add tag -> name:.pifindex
        DbHandler::AttribMap amap_name_pifindex;
        amap_name_pifindex.insert(std::make_pair("flow.pifindex", pifindex));
        tmap.insert(std::make_pair("name", std::make_pair(name,
                amap_name_pifindex)));
        // Add tag -> .sip
        DbHandler::AttribMap amap_sip;
        tmap.insert(std::make_pair("flow.sip", std::make_pair(sip, amap_sip)));
        // Add tag -> .dip
        DbHandler::AttribMap amap_dip;
        tmap.insert(std::make_pair("flow.dip", std::make_pair(dip, amap_dip)));
        // Add tag -> .protocol:.sport
        DbHandler::AttribMap amap_protocol_sport;
        amap_protocol_sport.insert(std::make_pair("flow.sport", sport));
        tmap.insert(std::make_pair("flow.protocol",
                std::make_pair(protocol, amap_protocol_sport)));
        // Add tag -> .protocol:.dport
        DbHandler::AttribMap amap_protocol_dport;
        amap_protocol_dport.insert(std::make_pair("flow.dport", dport));
        tmap.insert(std::make_pair("flow.protocol",
                std::make_pair(protocol, amap_protocol_dport)));
        StatTableInsert(timestamp, "UFlowData", "flow", tmap, amap, db_cb);
    }
    return true;
}

using namespace zookeeper::client;

DbHandlerInitializer::DbHandlerInitializer(EventManager *evm,
    const std::string &db_name, const std::string &timer_task_name,
    DbHandlerInitializer::InitializeDoneCb callback,
    const Options::Cassandra &cassandra_options,
    const std::string &zookeeper_server_list,
    bool use_zookeeper,
    const DbWriteOptions &db_write_options,
    ConfigClientCollector *config_client) :
    db_name_(db_name),
    db_handler_(new DbHandler(evm,
        boost::bind(&DbHandlerInitializer::ScheduleInit, this),
        db_name, cassandra_options,
        true, db_write_options, config_client)),
    callback_(callback),
    db_init_timer_(TimerManager::CreateTimer(*evm->io_service(),
        db_name + " Db Init Timer",
        TaskScheduler::GetInstance()->GetTaskId(timer_task_name))),
    zookeeper_server_list_(zookeeper_server_list),
    use_zookeeper_(use_zookeeper),
    zoo_locked_(false) {
    if (use_zookeeper_) {
        zoo_client_.reset(new ZookeeperClient(db_name_.c_str(),
            zookeeper_server_list_.c_str()));
        zoo_mutex_.reset(new ZookeeperLock(zoo_client_.get(), "/collector"));
    }
}

DbHandlerInitializer::DbHandlerInitializer(EventManager *evm,
    const std::string &db_name, const std::string &timer_task_name,
    DbHandlerInitializer::InitializeDoneCb callback,
    DbHandlerPtr db_handler) :
    db_name_(db_name),
    db_handler_(db_handler),
    callback_(callback),
    db_init_timer_(TimerManager::CreateTimer(*evm->io_service(),
        db_name + " Db Init Timer",
        TaskScheduler::GetInstance()->GetTaskId(timer_task_name))) {
}

DbHandlerInitializer::~DbHandlerInitializer() {
}

bool DbHandlerInitializer::Initialize() {
    // Synchronize creation across nodes using zookeeper
    if (use_zookeeper_ && !zoo_locked_) {
        assert(zoo_mutex_->Lock());
        zoo_locked_ = true;
    }
    if (!db_handler_->Init(true)) {
        if (use_zookeeper_ && zoo_locked_) {
            assert(zoo_mutex_->Release());
            zoo_locked_ = false;
        }
        // Update connection info
        ConnectionState::GetInstance()->Update(ConnectionType::DATABASE,
            db_name_, ConnectionStatus::DOWN, db_handler_->GetEndpoints(),
            std::string());
        LOG(DEBUG, db_name_ << ": Db Initialization FAILED");
        ScheduleInit();
        return false;
    }
    if (use_zookeeper_ && zoo_locked_) {
        assert(zoo_mutex_->Release());
        zoo_locked_ = false;
    }
    // Update connection info
    ConnectionState::GetInstance()->Update(ConnectionType::DATABASE,
        db_name_, ConnectionStatus::UP, db_handler_->GetEndpoints(),
        std::string());

    if (callback_) {
       callback_();
    }

    LOG(DEBUG, db_name_ << ": Db Initialization DONE");
    return true;
}

DbHandlerPtr DbHandlerInitializer::GetDbHandler() const {
    return db_handler_;
}

void DbHandlerInitializer::Shutdown() {
    TimerManager::DeleteTimer(db_init_timer_);
    db_init_timer_ = NULL;
    db_handler_->UnInit();
}

bool DbHandlerInitializer::InitTimerExpired() {
    // Start the timer again if initialization is not done
    bool done = Initialize();
    return !done;
}

void DbHandlerInitializer::InitTimerErrorHandler(string error_name,
    string error_message) {
    LOG(ERROR, db_name_ << ": " << error_name << " " << error_message);
}

void DbHandlerInitializer::StartInitTimer() {
    db_init_timer_->Start(kInitRetryInterval,
        boost::bind(&DbHandlerInitializer::InitTimerExpired, this),
        boost::bind(&DbHandlerInitializer::InitTimerErrorHandler, this,
                    _1, _2));
}

void DbHandlerInitializer::ScheduleInit() {
    db_handler_->UnInit();
    StartInitTimer();
}

// Prepend T2 in decimal since T2 timestamp column is in decimal
std::string PrependT2(uint32_t T2, const std::string &str) {
    std::string tempstr = integerToString(T2);
    tempstr.append(":");
    tempstr.append(str);
    return tempstr;
}
