//
// Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
//

#ifndef ANALYTICS_STRUCTURED_SYSLOG_COLLECTOR_H_
#define ANALYTICS_STRUCTURED_SYSLOG_COLLECTOR_H_

#include <string>
#include <vector>
#include <boost/scoped_ptr.hpp>
#include "structured_syslog_server.h"

class StructuredSyslogCollector {
 public:
    StructuredSyslogCollector(EventManager *evm, uint16_t port,
        const vector<string> &structured_syslog_tcp_forward_dst,
        const std::string &structured_syslog_kafka_broker,
        const std::string &structured_syslog_kafka_topic,
        uint16_t structured_syslog_kafka_partitions,
        uint64_t structured_syslog_active_session_map_limit,
        const Options::Kafka &kafka_options,
        DbHandlerPtr db_handler,
        ConfigClientCollector *config_client);
    virtual ~StructuredSyslogCollector();
    bool Initialize();
    void Shutdown();

 private:
    boost::scoped_ptr<structured_syslog::StructuredSyslogServer> server_;
};

#endif  // ANALYTICS_STRUCTURED_SYSLOG_COLLECTOR_H_
