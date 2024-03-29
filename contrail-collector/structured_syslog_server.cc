//
// Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
//

#include <utility>
#include <string>
#include <vector>
#include <boost/asio/buffer.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/algorithm/string.hpp>

#include <sandesh/sandesh_message_builder.h>

#include <base/logging.h>
#include "base/address_util.h"
#include <io/io_types.h>
#include <io/tcp_server.h>
#include <io/tcp_session.h>
#include <io/udp_server.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "structured_syslog_server.h"
#include "structured_syslog_server_impl.h"
#include "generator.h"
#include <analytics/sdwan_uve_types.h>
#include "syslog_collector.h"
#include "structured_syslog_config.h"


using std::make_pair;

namespace structured_syslog {

namespace impl {


void StructuredSyslogDecorate(SyslogParser::syslog_m_t &v, StructuredSyslogConfig *config_obj,
                              boost::shared_ptr<std::string> msg, std::vector<std::string> int_fields);
void StructuredSyslogPush(SyslogParser::syslog_m_t v, StatWalker::StatTableInsertFn stat_db_callback,
    std::vector<std::string> tagged_fields);
void StructuredSyslogUVESummarize(SyslogParser::syslog_m_t v, bool summarize_user, StructuredSyslogConfig *config_obj);
boost::shared_ptr<std::string> StructuredSyslogJsonMessage(SyslogParser::syslog_m_t v);

size_t DecorateMsg(boost::shared_ptr<std::string> msg, const std::string &key, const std::string &val, size_t prev_pos) {
    if (msg == NULL) {
        return 0;
    }
    size_t pos = msg->find(']', prev_pos);
    if (pos == std::string::npos) {
        return 0;
    }
    std::string insert_str = " " + key + "=\"" + val + "\"";
    msg->insert(pos, insert_str);
    return pos;
}

bool ParseStructuredPart(SyslogParser::syslog_m_t *v, const std::string &structured_part,
                         const std::vector<std::string> &int_fields, boost::shared_ptr<std::string> fwd_msg){
    std::size_t start = 0, end = 0;
    size_t prev_pos = 0;
    while ((end = structured_part.find('=', start)) != std::string::npos) {
        const std::string key = structured_part.substr(start, end - start);
        start = end + 2;
        end = structured_part.find('"', start);
        if (end == std::string::npos) {
            LOG(ERROR, "BAD structured_syslog: " << structured_part);
            return false;
          }
        const std::string val = structured_part.substr(start, end - start);
        LOG(DEBUG, "structured_syslog - " << key << " : " << val);
        start = end + 2;
        if (std::find(int_fields.begin(), int_fields.end(),
                      key) != int_fields.end()) {
            LOG(DEBUG, "int field - " << key);
            int64_t ival = atol(val.c_str());
            v->insert(std::pair<std::string, SyslogParser::Holder>(key,
                  SyslogParser::Holder(key, ival)));
        } else {
            v->insert(std::pair<std::string, SyslogParser::Holder>(key,
                  SyslogParser::Holder(key, val)));
        }
        prev_pos = DecorateMsg(fwd_msg, key, val, prev_pos);
  }
  return true;
}

bool filter_msg(SyslogParser::syslog_m_t &v) {
  std::string tag  = SyslogParser::GetMapVals(v, "tag", "UNKNOWN");
  std::string reason = SyslogParser::GetMapVals(v, "reason", "UNKNOWN");
  if (tag == "APPQOE_BEST_PATH_SELECTED" && 
    ((reason == "session close") || reason == "app detected")) {
    return false;
  }
  if (tag == "SNMP_TRAP_LINK_UP" || tag == "SNMP_TRAP_LINK_DOWN") {
    if (SyslogParser::GetMapVals(v, "role", "UNKNOWN") == "HUB" && 
      SyslogParser::GetMapVals(v,"interface-name", "UNKNOWN").compare(0, 2, "st") != 0) {
      return false;
    }
  }
  return true;
}

//filter out session close syslog for incoming traffic i.e. syslog coming from destination site.
bool filter_session_close_msg(SyslogParser::syslog_m_t &v) {
  std::string routing_instance = SyslogParser::GetMapVals(v, "routing-instance", "UNKNOWN");
  if (routing_instance.size() > 3 && ( routing_instance.compare(0,4,"LAN-") == 0)) {
      return true;
  }
  return false;
}

//filter out vol update syslog for incoming traffic i.e. syslog coming from destination site.
bool filter_vol_update_msg(SyslogParser::syslog_m_t &v) {
  std::string department = SyslogParser::GetMapVals(v, "source-zone-name", "UNKNOWN");
  if ((department.compare(0,5,"trust") == 0) || (department.compare(0,7,"untrust") == 0)) {
      return true;
  }
  return false;
}

bool StructuredSyslogPostParsing (SyslogParser::syslog_m_t &v, StructuredSyslogConfig *config_obj,
                                  StatWalker::StatTableInsertFn stat_db_callback, const uint8_t *message,
                                  int message_len, boost::shared_ptr<StructuredSyslogForwarder> forwarder){
  /*
  syslog format: <14>1 2016-12-06T11:38:19.818+02:00 csp-ucpe-bglr51 RT_FLOW: APPTRACK_SESSION_CLOSE [junos@2636.1.1.1.2.26
  reason="TCP RST" source-address="4.0.0.3" source-port="13175" destination-address="5.0.0.7"
  destination-port="48334" service-name="None" application="HTTP" nested-application="Facebook"
  nat-source-address="10.110.110.10" nat-source-port="13175" destination-address="96.9.139.213"
  nat-destination-port="48334" src-nat-rule-name="None" dst-nat-rule-name="None" protocol-id="6"
  policy-name="dmz-out" source-zone-name="DMZ" destination-zone-name="Internet" session-id-32="44292"
  packets-from-client="7" bytes-from-client="1421" packets-from-server="6" bytes-from-server="1133"
  elapsed-time="4" username="Frank" roles="Engineering" encrypted="No" profile-name="pf1" rule-name="1"
  routing-instance="inst1" destination-interface-name="xe-1/2/0.0"]
  */

  /*
  Remove unnecessary fields so that we avoid writing them into DB
  */
  v.erase("msglen");
  v.erase("severity");
  v.erase("facility");
  v.erase("year");
  v.erase("month");
  v.erase("day");
  v.erase("hour");
  v.erase("min");
  v.erase("sec");
  v.erase("msec");
  if (SyslogParser::GetMapVal(v, "pid", -1) == -1)
    v.erase("pid");

  const std::string body(SyslogParser::GetMapVals(v, "body", ""));
  std::size_t start = 0, end = 0;
  v.erase("body");
  LOG(DEBUG, "BODY: " << body);
  end = body.find('[', start);
  size_t tag_end = end;

  if (end == std::string::npos) {
    LOG(ERROR, "BAD structured_syslog: " << body);
    return false;
  }
  end = body.find(']', end+1);

  if (end == std::string::npos) {
    LOG(ERROR, "BAD structured_syslog: " << body);
    return false;
  }
  end = body.find(' ', start);

  if (end == std::string::npos) {
    LOG(ERROR, "BAD structured_syslog: " << body);
    return false;
  }
  std::string tag_ = "UNKNOWN";
  std::string find_tag_str = body.substr(start,tag_end-1);
  std::size_t tag_index = find_tag_str.find_last_of(' ');
  if (tag_index !=  std::string::npos){
      tag_  = find_tag_str.substr(tag_index+1, end-tag_index);
  }
  else {tag_ = find_tag_str;}
  const std::string tag = tag_;
  LOG(DEBUG, "structured_syslog - tag: " << tag );
  boost::shared_ptr<MessageConfig> mc = config_obj->GetMessageConfig(tag);
  if (mc == NULL || ((mc->process_and_store() == false) && (mc->forward() == false)
     && (mc->process_and_summarize() == false))) {
    LOG(DEBUG, "structured_syslog - not processing message: " << tag );
    return false;
  }
  LOG(DEBUG, "structured_syslog - message_config: " << mc->name());
  boost::shared_ptr<std::string> msg;
  boost::shared_ptr<std::string> hostname;
  start = tag_end;

  v.insert(std::pair<std::string, SyslogParser::Holder>("tag",
        SyslogParser::Holder("tag", tag)));

  end = body.find(' ', start);

  if (end == std::string::npos) {
    LOG(ERROR, "BAD structured_syslog: " << body);
    return false;
  }
  const std::string hardware = body.substr(start+1, end-start-1);

  start = end + 1;
  LOG(DEBUG, "structured_syslog - hardware: " << hardware);
  v.insert(std::pair<std::string, SyslogParser::Holder>("hardware",
        SyslogParser::Holder("hardware", hardware)));
  end = body.find_last_of(']');
  std::string structured_part = body.substr(start, end-start);
  LOG(DEBUG, "structured_syslog - struct_data: " << structured_part);

  bool ret = ParseStructuredPart(&v, structured_part, mc->ints(), msg);
  if (ret == false){
    return ret;
  }
  // Do not process APPTRACK_SESSION_CLOSE syslogs for incoming traffic 
  if(tag == "APPTRACK_SESSION_CLOSE" && filter_session_close_msg(v)){
    return false;
  }
  
  // Do not process APPTRACK_SESSION_VOL_UPDATE syslogs for incoming traffic. 
  if(tag == "APPTRACK_SESSION_VOL_UPDATE" && filter_vol_update_msg(v)) {
    return false;
  }

  if (mc->process_and_store() == true || mc->process_before_forward() == true
      || mc->process_and_summarize() == true) {
      if (forwarder != NULL && mc->forward() == true) {
        msg.reset(new std::string (message, message + message_len));
        hostname.reset(new std::string (SyslogParser::GetMapVals(v, "hostname", "")));
      }
      StructuredSyslogDecorate(v, config_obj, msg, mc->ints());
      if (mc->process_and_summarize() == true) {
        bool syslog_summarize_user = mc->process_and_summarize_user();
        StructuredSyslogUVESummarize(v, syslog_summarize_user, config_obj);
      }
      if (forwarder != NULL && mc->forward() == true &&
          mc->process_before_forward() == true && filter_msg (v)) {
        std::stringstream msglength;
        msglength << msg->length();
        msg->insert(0, msglength.str() + " ");
        LOG(DEBUG, "forwarding after decoration - " << *msg);
        boost::shared_ptr<std::string> json_msg;
        if (forwarder->kafkaForwarder()) {
          json_msg = StructuredSyslogJsonMessage(v);
        }
        boost::shared_ptr<StructuredSyslogQueueEntry> ssqe(new StructuredSyslogQueueEntry(msg, msg->length(),
                                                                                          json_msg, hostname));
        forwarder->Forward(ssqe);
      }
      if (mc->process_and_store() == true) {
        StructuredSyslogPush(v, stat_db_callback, mc->tags());
      }
  }
  if (forwarder != NULL && mc->forward() == true && filter_msg(v) &&
      mc->process_before_forward() == false) {
    msg.reset(new std::string (message, message + message_len));
    hostname.reset(new std::string (SyslogParser::GetMapVals(v, "hostname", "")));
    std::stringstream msglength;
    msglength << msg->length();
    msg->insert(0, msglength.str() + " ");
    LOG(DEBUG, "forwarding without decoration - " << *msg);
    boost::shared_ptr<std::string> json_msg;
    if (forwarder->kafkaForwarder()) {
        json_msg = StructuredSyslogJsonMessage(v);
    }
    boost::shared_ptr<StructuredSyslogQueueEntry> ssqe(new StructuredSyslogQueueEntry(msg, msg->length(),
                                                                                      json_msg, hostname));
    forwarder->Forward(ssqe);
  }
  return true;
}

static inline void PushStructuredSyslogAttribsAndTags(DbHandler::AttribMap *attribs,
    StatWalker::TagMap *tags, bool is_tag, const std::string &name,
    DbHandler::Var value) {
    // Insert into the attribute map
    attribs->insert(make_pair(name, value));
    if (is_tag) {
        // Insert into the tag map
        StatWalker::TagVal tvalue;
        tvalue.val = value;
        tags->insert(make_pair(name, tvalue));
    }
}

void PushStructuredSyslogStats(SyslogParser::syslog_m_t v, const std::string &stat_attr_name,
                               StatWalker *stat_walker, std::vector<std::string> tagged_fields) {
    // At the top level the stat walker already has the tags so
    // we need to skip going through the elemental types and
    // creating the tag and attribute maps. At lower levels,
    // only strings are inserted into the tag map
    bool top_level(stat_attr_name.empty());
    DbHandler::AttribMap attribs;
    StatWalker::TagMap tags;

    if (!top_level) {

      int i = 0;
      while (!v.empty()) {
      /*
      All the key-value pairs in v will be iterated over and pushed into the stattable
      */
          SyslogParser::Holder d = v.begin()->second;
          const std::string &key(d.key);
          bool is_tag = false;
           if (std::find(tagged_fields.begin(), tagged_fields.end(), key) != tagged_fields.end()) {
            LOG(DEBUG, "tagged field - " << key);
            is_tag = true;
           }

          if (d.type == SyslogParser::str_type) {
            const std::string &sval(d.s_val);
            LOG(DEBUG, i++ << " - " << key << " : " << sval << " is_tag: " << is_tag);
            DbHandler::Var svalue(sval);
            PushStructuredSyslogAttribsAndTags(&attribs, &tags, is_tag, key, svalue);
          }
          else if (d.type == SyslogParser::int_type) {
            LOG(DEBUG, i++ << " - " << key << " : " << d.i_val << " is_tag: " << is_tag);
            if (d.i_val >= 0) { /* added this condition as having -ve values was resulting in a crash */
                DbHandler::Var ivalue(static_cast<uint64_t>(d.i_val));
                PushStructuredSyslogAttribsAndTags(&attribs, &tags, is_tag, key, ivalue);
            }
          }
          else {
            LOG(ERROR, i++ << "BAD Type: ");
          }
          v.erase(v.begin());
      }

      // Push the stats at this level
      stat_walker->Push(stat_attr_name, tags, attribs);

    }
    // Perform traversal of children
    else {
        PushStructuredSyslogStats(v, "data", stat_walker, tagged_fields);
    }

    // Pop the stats at this level
    if (!top_level) {
        stat_walker->Pop();
    }
}

void PushStructuredSyslogTopLevelTags(SyslogParser::syslog_m_t v, StatWalker::TagMap *top_tags) {
    StatWalker::TagVal tvalue;
    const std::string ip(SyslogParser::GetMapVals(v, "ip", ""));
    const std::string saddr(SyslogParser::GetMapVals(v, "hostname", ip));
    tvalue.val = saddr;
    top_tags->insert(make_pair("Source", tvalue));
}

void StructuredSyslogPush(SyslogParser::syslog_m_t v, StatWalker::StatTableInsertFn stat_db_callback,
    std::vector<std::string> tagged_fields) {
    StatWalker::TagMap top_tags;
    PushStructuredSyslogTopLevelTags(v, &top_tags);
    StatWalker stat_walker(stat_db_callback, (uint64_t)SyslogParser::GetMapVal (v, "timestamp", 0),
                           "JunosSyslog", top_tags);
    PushStructuredSyslogStats(v, std::string(), &stat_walker, tagged_fields);
}

boost::shared_ptr<std::string>
StructuredSyslogJsonMessage(SyslogParser::syslog_m_t v) {
    contrail_rapidjson::Document doc;
    doc.SetObject();
    for (std::map<std::string, SyslogParser::Holder>::iterator i=v.begin(); i!=v.end(); ++i) {
        SyslogParser::Holder val = i->second;
        const std::string &key(val.key);
        if (val.type == SyslogParser::str_type) {
            contrail_rapidjson::Value vk;
            contrail_rapidjson::Value value(contrail_rapidjson::kStringType);
            value.SetString(val.s_val.c_str(), doc.GetAllocator());
            doc.AddMember(vk.SetString(key.c_str(),
                                 doc.GetAllocator()), value, doc.GetAllocator());
        }
        else if (val.type == SyslogParser::int_type) {
            contrail_rapidjson::Value vk;
            contrail_rapidjson::Value value(contrail_rapidjson::kNumberType);
            value.SetUint64(val.i_val);
            doc.AddMember(vk.SetString(key.c_str(),
                                 doc.GetAllocator()), value, doc.GetAllocator());
        }
    }
    contrail_rapidjson::StringBuffer buffer;
    contrail_rapidjson::Writer<contrail_rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    boost::shared_ptr<std::string> json_msg(new std::string(buffer.GetString()));
    return json_msg;
}

//Identify the prefix and return the VPN name if nothing matches then return routing_instance
const std::string get_VPNName(const std::string &routing_instance){
    if (routing_instance.size() > 16 && (routing_instance.compare(0, 16, "Default-reverse-") == 0)){
        if (routing_instance.size() > 20 && (routing_instance.compare(16, 4, "hub-") == 0)){
            return routing_instance.substr(20, (routing_instance.size() - 20));
        }
        return routing_instance.substr(16, (routing_instance.size() - 16));
    }
    if (routing_instance.size() > 8 && (routing_instance.compare(0, 8,"Default-") == 0)){
        if (routing_instance.size() > 12 && (routing_instance.compare(8, 4, "hub-") == 0)){
            return routing_instance.substr(12, (routing_instance.size() - 12));
        }
        return routing_instance.substr(8, (routing_instance.size() - 8));
   }
    if (routing_instance.size() > 5 && (routing_instance.compare(0, 5, "mpls-") == 0)){
        if (routing_instance.size() > 9 && (routing_instance.compare(5, 4, "hub-") == 0)){
            return routing_instance.substr(9, (routing_instance.size() - 9));
        }
        return routing_instance.substr(5, (routing_instance.size() - 5));
    }
    if (routing_instance.size() > 9 && ( routing_instance.compare(0, 9, "internet-") == 0)){
        if (routing_instance.size() > 13 && (routing_instance.compare(9, 4, "hub-") == 0)){
            return routing_instance.substr(13, (routing_instance.size() - 13));
        }
        return routing_instance.substr(9, (routing_instance.size() - 9));
    }
    return routing_instance;
}

//Identify the VPN name from department depending upon the case
// in which network segmentation is enabled or disabled.
const std::string get_VPNName(const std::string &department, 
                            const std::string &network_segmentation, 
                            const std::string &tenant_name) {
    std::string vpn_name = department;
    if (boost::iequals(network_segmentation, "Disabled") || boost::iequals(department, "Default")) {
            vpn_name = tenant_name + "_DefaultVPN";
    }
    return vpn_name;
}


void StructuredSyslogUVESummarizeData(SyslogParser::syslog_m_t v, bool summarize_user, StructuredSyslogConfig *config_obj) {
    SDWANMetricsRecord sdwanmetricrecord;
    SDWANTenantMetricsRecord sdwantenantmetricrecord;
    SDWANKPIMetricsRecord sdwankpimetricrecord_source;

    const std::string tag(SyslogParser::GetMapVals(v, "tag", "UNKNOWN"));
    const std::string process_vol_update(SyslogParser::GetMapVals(v, "process-vol-update", "False"));
    // process_vol_update as True enables diff calculation
    // from cumulative counters across sequence of traffic syslogs.
    // Note that VOL_UPDATE should be considered for counters with SLA-PROFILE
    // or TRAFFIC-TYPE because the VOL_UPDATE doesn't contain such information.
    LOG(DEBUG, "StructuredSyslogUVESummarizeData - process-vol-update : " << process_vol_update);
    bool is_close = boost::equals(tag, "APPTRACK_SESSION_CLOSE");
    bool is_vol_update = boost::equals(tag, "APPTRACK_SESSION_VOL_UPDATE");

    if (!boost::iequals(process_vol_update, "True")) {
        /*
        For certain sites VOL_UPDATE syslog may not supported.
        For such sites only SESSION_CLOSE syslog should be used.
        */
        if (is_vol_update) {
            // reject vol_update syslogs.
            LOG(DEBUG, "StructuredSyslogUVESummarizeData - VOL_UPDATE msg rejected.");
            return;
        }
    }
    

    const std::string location(SyslogParser::GetMapVals(v, "location", "UNKNOWN"));
    const std::string tenant(SyslogParser::GetMapVals(v, "tenant", "UNKNOWN"));
    std::string sla_profile(SyslogParser::GetMapVals(v, "sla-profile", "UNKNOWN"));
    const std::string app_category(SyslogParser::GetMapVals(v, "app-category", "UNKNOWN"));
    const std::string department(SyslogParser::GetMapVals(v, "source-zone-name", "UNKNOWN"));
    const std::string dest_zone(SyslogParser::GetMapVals(v, "destination-zone-name", "UNKNOWN"));
    const std::string device_id(SyslogParser::GetMapVals(v, "device", "UNKNOWN"));
    const std::string region(SyslogParser::GetMapVals(v, "region", "DEFAULT"));
    const std::string opco(SyslogParser::GetMapVals(v, "OPCO", "DEFAULT"));
    const std::string hubs_interfaces(SyslogParser::GetMapVals(v, "HUBS", "UNKNOWN"));
    const std::string uvename = tenant + "::" + location + "::" + device_id;
    const std::string tenantuvename = region + "::" + opco + "::" + tenant;
    const std::string kpi_uvename_source = location;
    std::string traffic_type(SyslogParser::GetMapVals(v, "active-probe-params", "UNKNOWN"));
    const std::string dscp_alias_code(SyslogParser::GetMapVals(v, "dscp-alias-code", "UNKNOWN"));
    const std::string dscp_value(SyslogParser::GetMapVals(v, "dscp-value", "UNKNOWN"));
    std::string nested_appname(SyslogParser::GetMapVals(v, "nested-application", "UNKNOWN"));
    std::string service_name(SyslogParser::GetMapVals(v, "service-name", "UNKNOWN"));
    std::string appname(SyslogParser::GetMapVals(v, "application", "UNKNOWN"));
    const std::string tenant_name(SyslogParser::GetMapVals(v, "tenantaddr", "UNKNOWN"));
    const std::string network_segmentation(SyslogParser::GetMapVals(v, "network-segmentation", "UNKNOWN"));
    bool is_site_traffic_destination = true;

    //counters for cumulative to differential value conversation
    int64_t prev_total_bytes=0, prev_bytes_from_client=0,
            prev_bytes_from_server=0, prev_packets_from_client=0,
            prev_packets_from_server=0;
    bool process_curr_counter = true;

    if (boost::iequals(process_vol_update, "True")) {
        // ToDo: Remove this once vol-update syslog provides sla-profile.
        sla_profile = "DEFAULT";
        // ToDo: Remove this once vol-update syslog provides traffic-type.
        traffic_type = "DEFAULT";
    }

    /*
    If VOL_UPDATE syslogs were processed, compute diff values
    from cumulative traffic counters for each session-id.
    Note that prev counters for the session has to be checked in all cases
    to handle shift from
    cumulative -> diff computation (in case of process vol-update)
    to just diff computation (in case of session-close only).
    */
    const std::string session_id_32(SyslogParser::GetMapVals(v, "session-id-32", "-1"));
    const std::string session_unique_key = uvename + "::" + session_id_32;
    std::map<std::string, uint64_t> session_prev_traffic_counters;
    bool found_session_prev_counters =
            config_obj->FetchSyslogSessionCounters(session_unique_key,
                                            session_prev_traffic_counters);
    if (found_session_prev_counters) {
        // read previous counters
        prev_total_bytes = session_prev_traffic_counters["total-bytes"];
        prev_bytes_from_client = session_prev_traffic_counters["bytes-from-client"];
        prev_bytes_from_server = session_prev_traffic_counters["bytes-from-server"];
        prev_packets_from_server = session_prev_traffic_counters["packets-from-server"];
        prev_packets_from_client = session_prev_traffic_counters["packets-from-client"];

        // update & process the counter which are new and greater than before.
        if ((prev_total_bytes > SyslogParser::GetMapVal(v, "total-bytes", 0)) ||
            (prev_packets_from_client > SyslogParser::GetMapVal(v, "packets-from-client", 0))) {
                process_curr_counter = false;
                LOG(ERROR, "Syslog Session Counter is OLD !. Discarding ...");
        }

        if (is_close) {
            // session is closed.
            // Remove session counters from syslog session counter map.
            int removed_sess_counter =
                config_obj->RemoveSyslogSessionCounter(session_unique_key);
            if (removed_sess_counter == 0) {
                LOG(ERROR, "Syslog Session Counter NOT removed for key " << session_unique_key);
            }
        }
    }

    // either this is a new session,
    // or this update has new cumulative counter count.
    // add/update counters to syslog session counter map if
    // processing vol update is enabled.
    if (!is_close && boost::iequals(process_vol_update, "True")) {
        if (process_curr_counter) {
            std::map<std::string, uint64_t> session_curr_traffic_counters;
            session_curr_traffic_counters["total-bytes"] = SyslogParser::GetMapVal(v, "total-bytes", 0);
            session_curr_traffic_counters["bytes-from-client"] = SyslogParser::GetMapVal(v, "bytes-from-client", 0);
            session_curr_traffic_counters["bytes-from-server"] = SyslogParser::GetMapVal(v, "bytes-from-server", 0);
            session_curr_traffic_counters["packets-from-server"] = SyslogParser::GetMapVal(v, "packets-from-server", 0);
            session_curr_traffic_counters["packets-from-client"] = SyslogParser::GetMapVal(v, "packets-from-client", 0);
            bool addSessionCounter = config_obj->AddSyslogSessionCounter(session_unique_key, session_curr_traffic_counters);
            if (!addSessionCounter) {
                LOG(ERROR, "StructuredSyslogUVESummarizeData - Syslog message rejected.");
                return;
            }

        }
    }


    // compute diff counters
    //LOG(DEBUG, "prev-total-bytes: " << prev_total_bytes);
    int64_t diff_total_bytes=0, diff_bytes_from_client=0,
            diff_bytes_from_server=0, diff_packets_from_server=0,
            diff_packets_from_client=0;

    if (process_curr_counter) {
        diff_total_bytes = SyslogParser::GetMapVal(v, "total-bytes", 0) - prev_total_bytes;
        diff_bytes_from_client = SyslogParser::GetMapVal(v, "bytes-from-client", 0) - prev_bytes_from_client;
        diff_bytes_from_server = SyslogParser::GetMapVal(v, "bytes-from-server", 0) - prev_bytes_from_server;
        diff_packets_from_server = SyslogParser::GetMapVal(v, "packets-from-server", 0) - prev_packets_from_server;
        diff_packets_from_client = SyslogParser::GetMapVal(v, "packets-from-client", 0) - prev_packets_from_client;
    }
    LOG(DEBUG, "Diff total-bytes: " << diff_total_bytes);
    LOG(DEBUG, "Diff bytes-from-client: " << diff_bytes_from_client);
    LOG(DEBUG, "Diff bytes-from-server: " << diff_bytes_from_server);
    LOG(DEBUG, "Diff packets-from-server: " << diff_packets_from_server);
    LOG(DEBUG, "Diff packets-from-client: " << diff_packets_from_client);

    LOG(DEBUG, "UVE: dscp-alias-code: " << dscp_alias_code);
    LOG(DEBUG, "UVE: dscp-value: " << dscp_value);

    if (boost::iequals(nested_appname, "UNKNOWN") && boost::iequals(appname, "UNKNOWN")) {
        if (!(boost::iequals(service_name, "UNKNOWN")) && !(boost::iequals(service_name, "None"))) {
            nested_appname =  service_name;
            appname = service_name;
        }
    }

    // nested_appname@UNKNOWN, nested_appname@dscp_alias_code nested_appname@DSCP-dscp_value
    std::string dscp_key = dscp_alias_code;
    if (dscp_value == "UNKNOWN") {
        dscp_key = "UNKNOWN";
    }
    else if (dscp_alias_code == "UNKNOWN") {
        dscp_key = "DSCP-" + dscp_value;
    }

    const std::string nested_appname_with_alias_code = nested_appname + "@" + dscp_key;
    const std::string tt_app_dept_info = traffic_type + "(" + nested_appname_with_alias_code + ":" + appname
                                         + "/" + app_category +  ")" + "::" + department + "::";
                                         
    //username => syslog.username or syslog.source-address
    std::string username(SyslogParser::GetMapVals(v, "username", "UNKNOWN"));
    if (boost::iequals(username, "unknown")) {
        username = SyslogParser::GetMapVals(v, "source-address", "UNKNOWN");
    }

    sdwanmetricrecord.set_name(uvename);
    sdwantenantmetricrecord.set_name(tenantuvename);
    sdwankpimetricrecord_source.set_name(kpi_uvename_source);

    //Update maps for SDWANKPI metrics record
    SDWANKPIMetrics_diff sdwankpimetricdiff;
    std::map<std::string, SDWANKPIMetrics_diff> sdwan_kpi_metrics_diff_source;
    if ((is_close || is_vol_update) && (boost::iequals(dest_zone, "trust"))) {
        const std::string routing_instance (SyslogParser::GetMapVals(v, "routing-instance", "UNKNOWN"));
        std::string vpn_name = "UNKNOWN";
        if (routing_instance != "UNKNOWN") {
            vpn_name = get_VPNName(routing_instance);
        }
        else {
            // this case will be true for vol_update syslogs.
            vpn_name = 
                get_VPNName(department, network_segmentation, tenant_name);
        }

        const std::string destination_address (SyslogParser::GetMapVals(v, "destination-address", "UNKNOWN"));
        const std::string destination_interface_name (SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN"));
        if (is_close)
        {
            sdwankpimetricdiff.set_session_close_count(1);
        }
        else if (is_vol_update)
        {   
            sdwankpimetricdiff.set_bps(diff_total_bytes);
        }
        
        std::string destination_site;
        
        //Find Network only if valid VPN name is parsed from routing instance
        if (vpn_name != routing_instance){
            std::string network_key = tenant + "::" + vpn_name;
            destination_site = config_obj->FindNetwork(destination_address, network_key, location);
        }
        // if VPN name == routing instance, then VPN name is not valid and assign destination site to "UNKNOWN"
        if ((vpn_name == routing_instance) || destination_site.empty() ){
            destination_site = "UNKNOWN";
            is_site_traffic_destination = false;
        }
        LOG(DEBUG,"destination address "<< destination_address  <<" in VPN " << vpn_name <<
         " belongs to site : " << destination_site);

        std::string searchHubInterface = destination_interface_name + ",";
        std::size_t destination_interface_name_found = hubs_interfaces.find(searchHubInterface);

         // map key should be destination site for source UVE and source site for destination UVE
        std::string kpimetricdiff_key_source(destination_site);
        LOG(DEBUG,"UVE: KPI key destination : " << kpimetricdiff_key_source);
        sdwan_kpi_metrics_diff_source.insert(std::make_pair(kpimetricdiff_key_source, sdwankpimetricdiff));

        // If destination interface name belongs to hub_interfaces then traffic goes via HUB 
        if (destination_interface_name_found != std::string::npos){
            LOG(DEBUG,"destination_interface_name "<< destination_interface_name <<" found in hubs_interfaces" );

            sdwankpimetricrecord_source.set_kpi_metrics_greater_diff(sdwan_kpi_metrics_diff_source);
        }
        else {
            LOG(DEBUG,"destination_interface_name "<< destination_interface_name <<" NOT found in hubs_interfaces" );

            sdwankpimetricrecord_source.set_kpi_metrics_lesser_diff(sdwan_kpi_metrics_diff_source);
        }
    }//End of Update maps for SDWANKPI metrics record
    SDWANKPIMetrics::Send(sdwankpimetricrecord_source,"ObjectCPETable");

    std::string link1, link2, link1_info, link2_info, traffic_destination_link1, traffic_destination_link2, link_type_link1, link_type_link2;
    int64_t link1_bytes = SyslogParser::GetMapVal(v, "uplink-tx-bytes", -1);
    int64_t link2_bytes = SyslogParser::GetMapVal(v, "uplink-rx-bytes", -1);


    if (link1_bytes < 0 || (boost::iequals(process_vol_update, "True"))) {
        link1_bytes = diff_bytes_from_client;
    }
    if (link2_bytes < 0 || (boost::iequals(process_vol_update, "True"))) {
        link2_bytes = diff_bytes_from_server;
    }

    if (is_close || is_vol_update) {
        // Fetch Interfaces for VOL_UPDATE or SESSION_CLOSE syslogs.

        link1 = (SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN"));
        //vol-update syslogs does NOT contain uplink interfaces and counters.
        if (!(boost::iequals(process_vol_update, "True"))) {
            link2 = (SyslogParser::GetMapVals(v, "uplink-incoming-interface-name", "N/A"));
        }
        else {link2 = "N/A";}

        std::string underlay_link1 = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
        link_type_link1 = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
        traffic_destination_link1 = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
        std::string metadata_link1 = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));

        if ((traffic_destination_link1 == "HUB" || traffic_destination_link1 == "EHUB") 
            && (is_site_traffic_destination)) {
            traffic_destination_link1 = traffic_destination_link1 + "2S" ;
        }
        else if (traffic_destination_link1 == "HUB" || traffic_destination_link1 == "EHUB") {
            traffic_destination_link1 = traffic_destination_link1 + "2CBO" ;
        }

        link1_info = link1 + "@" + underlay_link1
            + "@" + link_type_link1 + "@" + traffic_destination_link1 + "@" + metadata_link1 ;

        if (boost::iequals(link2, "N/A")) {
            link2 = link1;
            //link1_bytes = SyslogParser::GetMapVal(v, "bytes-from-client", 0);
            //link2_bytes = SyslogParser::GetMapVal(v, "bytes-from-server", 0);
            link1_bytes = diff_bytes_from_client;
            link2_bytes = diff_bytes_from_server;
            link2_info = link1_info;
        }
        else {
            std::string underlay_link2 = (SyslogParser::GetMapVals(v, "underlay-uplink-incoming-interface-name", "UNKNOWN"));
            link_type_link2 = (SyslogParser::GetMapVals(v, "link-type-uplink-incoming-interface-name", "UNKNOWN"));
            traffic_destination_link2 = (SyslogParser::GetMapVals(v, "traffic-destination-uplink-incoming-interface-name", "UNKNOWN"));
            std::string metadata_link2 = (SyslogParser::GetMapVals(v, "metadata-uplink-incoming-interface-name", "UNKNOWN"));
            link2_info = link2 + "@" + underlay_link2
                + "@" + link_type_link2 + "@" + traffic_destination_link2 + "@" + metadata_link2 ;
        }
        
        LOG(DEBUG,"UVE: link1_info :" << link1_info);
        LOG(DEBUG,"UVE: link2_info :" << link2_info);
    } else {
        // Fetch Interfaces for RT_FLOW_NEXTHOP_CHANGE syslog.

        if (!(boost::iequals(process_vol_update, "True"))) {
            link1 = (SyslogParser::GetMapVals(v, "last-destination-interface-name", "UNKNOWN"));
            link2 = (SyslogParser::GetMapVals(v, "last-incoming-interface-name", "UNKNOWN"));

            std::string underlay_link1 = (SyslogParser::GetMapVals(v, "underlay-last-destination-interface-name", "UNKNOWN"));
            link_type_link1 = (SyslogParser::GetMapVals(v, "link-type-last-destination-interface-name", "UNKNOWN"));
            traffic_destination_link1 = (SyslogParser::GetMapVals(v, "traffic-destination-last-destination-interface-name", "UNKNOWN"));
            std::string metadata_link1 = (SyslogParser::GetMapVals(v, "metadata-last-destination-interface-name", "UNKNOWN"));

            link1_info = link1 + "@" + underlay_link1
              + "@" + link_type_link1 + "@" + traffic_destination_link1 + "@" + metadata_link1 ;

            std::string underlay_link2 = (SyslogParser::GetMapVals(v, "underlay-last-incoming-interface-name", "UNKNOWN"));
            link_type_link2 = (SyslogParser::GetMapVals(v, "link-type-last-incoming-interface-name", "UNKNOWN"));
            traffic_destination_link2 = (SyslogParser::GetMapVals(v, "traffic-destination-last-incoming-interface-name", "UNKNOWN"));
            std::string metadata_link2 = (SyslogParser::GetMapVals(v, "metadata-last-incoming-interface-name", "UNKNOWN"));

            link2_info = link2 + "@" + underlay_link2
                + "@" + link_type_link2 + "@" + traffic_destination_link2 + "@" + metadata_link2 ;
        }
        else {
            link1 = SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN");
            link2 = link1;

            std::string underlay_link1 = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
            link_type_link1 = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
            traffic_destination_link1 = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
            std::string metadata_link1 = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));

            link1_info = link1 + "@" + underlay_link1
              + "@" + link_type_link1 + "@" + traffic_destination_link1 + "@" + metadata_link1 ;
            
            link2_info = link1_info;
        }

        LOG(DEBUG,"UVE: link1_info :" << link1_info);
        LOG(DEBUG,"UVE: link2_info :" << link2_info);
    }
    SDWANMetrics_diff sdwanmetric;
    if (is_close || is_vol_update) {
        //int64_t output_pkts = SyslogParser::GetMapVal(v, "packets-from-client", 0);
        //int64_t input_pkts = SyslogParser::GetMapVal(v, "packets-from-server", 0);
        int64_t output_pkts = diff_packets_from_client;
        int64_t input_pkts = diff_packets_from_server;
        sdwanmetric.set_total_pkts(input_pkts + output_pkts);
        sdwanmetric.set_input_pkts(input_pkts);
        sdwanmetric.set_output_pkts(output_pkts);
        sdwanmetric.set_total_bytes(diff_total_bytes);
        sdwanmetric.set_output_bytes(diff_bytes_from_client);
        sdwanmetric.set_input_bytes(diff_bytes_from_server);
        if (is_close) {
            sdwanmetric.set_session_duration(SyslogParser::GetMapVal(v, "elapsed-time", 0));
            sdwanmetric.set_session_count(1);
        }
        
        // Map: app_metrics_diff_sla
        std::map<std::string, SDWANMetrics_diff> app_metrics_diff_sla;
        std::string slamap_key(tt_app_dept_info + sla_profile);
        LOG(DEBUG,"UVE: app_metrics_diff_sla key :" << slamap_key);
        app_metrics_diff_sla.insert(std::make_pair(slamap_key, sdwanmetric));
        sdwanmetricrecord.set_app_metrics_diff_sla(app_metrics_diff_sla);

        // Map: app_metrics_diff_user
        if (summarize_user == true) {
            std::map<std::string, SDWANMetrics_diff> app_metrics_diff_user;
            std::string usermap_key(tt_app_dept_info + username);
            LOG(DEBUG,"UVE: app_metrics_diff_user key :" << usermap_key);
            app_metrics_diff_user.insert(std::make_pair(usermap_key, sdwanmetric));
            sdwanmetricrecord.set_app_metrics_diff_user(app_metrics_diff_user);
        }
    //replaced the fuction to the outerloop for both session_close and rt_flow_next_hop_change syslog
    // // Map: tenant_metrics_diff_sla
    // std::map<std::string, SDWANMetrics_diff> tenant_metrics_diff_sla;
    // std::string tenantmetric_key(location + "::" + sla_profile + "::" + traffic_type + "@" + traffic_destination_link + "@" + link_type_link);
    // LOG(DEBUG,"UVE: tenant_metrics_diff_sla key :" << tenantmetric_key);
    // tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric));
    // sdwantenantmetricrecord.set_tenant_metrics_diff_sla(tenant_metrics_diff_sla);
    // SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");
    }
    // Map: app_metrics_diff_link
    // Map: link_metrics_diff_traffic_type
    SDWANMetrics_diff sdwanmetric1;
    SDWANMetrics_diff sdwanmetric2;
    std::map<std::string, SDWANMetrics_diff> app_metrics_diff_link;
    std::map<std::string, SDWANMetrics_diff> link_metrics_diff_traffic_type;
    if (boost::equals(link1, link2)) {
        sdwanmetric1.set_total_bytes(link1_bytes + link2_bytes);
        sdwanmetric1.set_input_bytes(link2_bytes);
        sdwanmetric1.set_output_bytes(link1_bytes);
        if (is_close) {
            sdwanmetric1.set_session_duration(SyslogParser::GetMapVal(v, "elapsed-time", 0));
            sdwanmetric1.set_session_count(1);
        }
        std::string linkmap_key(tt_app_dept_info + link1_info);
        std::string linkmetricmap_key(link1_info + "::" + sla_profile + "::" + traffic_type);
        LOG(DEBUG,"UVE: app_metrics_diff_link key :" << linkmap_key);
        app_metrics_diff_link.insert(std::make_pair(linkmap_key, sdwanmetric1));
        sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);

        // Map: tenant_metrics_diff_sla
        std::map<std::string, SDWANMetrics_diff> tenant_metrics_diff_sla;
        LOG(DEBUG,"UVE: link_metrics_*_traffic_type key :" << linkmetricmap_key);
        link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key, sdwanmetric1));
        sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);

        // Update Map: tenant_metrics_diff_sla
        std::string tenantmetric_key(location + "::" + sla_profile +
                                    "::" + traffic_type +
                                    "@" + traffic_destination_link1 +
                                    "@" + link_type_link1);

        LOG(DEBUG,"UVE: tenant_metrics_diff_sla key :" << tenantmetric_key);
        tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric1));
        sdwantenantmetricrecord.set_tenant_metrics_diff_sla(tenant_metrics_diff_sla);




        /*
        // Update maps for underlay links if needed
        if (!(boost::equals(link1, underlay_link1))) {
            std::string linkmap_key(tt_app_dept_info + underlay_link1);
            std::string linkmetricmap_key(underlay_link1 + "::" + sla_profile + "::" + traffic_type);
            LOG(DEBUG,"UVE: underlay app_metrics_diff_link key :" << linkmap_key);
            LOG(DEBUG,"UVE: underlay link_metrics_*_traffic_type key :" << linkmetricmap_key);
            app_metrics_diff_link.insert(std::make_pair(linkmap_key, sdwanmetric1));
            link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key, sdwanmetric1));
            sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);
            sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);
        }
        */
    } else {
        sdwanmetric1.set_total_bytes(link1_bytes);
        sdwanmetric1.set_output_bytes(link1_bytes);
        sdwanmetric1.set_input_bytes(0);
        sdwanmetric2.set_total_bytes(link2_bytes);
        sdwanmetric2.set_output_bytes(0);
        sdwanmetric2.set_input_bytes(link2_bytes);
        if (is_close) {
            sdwanmetric1.set_session_duration(SyslogParser::GetMapVal(v, "elapsed-time", 0));
            sdwanmetric1.set_session_count(1);
            sdwanmetric2.set_session_duration(SyslogParser::GetMapVal(v, "elapsed-time", 0));
            sdwanmetric2.set_session_count(1);
        }

        std::string linkmap_key1(tt_app_dept_info + link1_info);
        std::string linkmap_key2(tt_app_dept_info + link2_info);
        LOG(DEBUG,"UVE: app_metrics_diff_link key1 :" << linkmap_key1);
        LOG(DEBUG,"UVE: app_metrics_diff_link key2 :" << linkmap_key2);
        app_metrics_diff_link.insert(std::make_pair(linkmap_key1, sdwanmetric1));
        app_metrics_diff_link.insert(std::make_pair(linkmap_key2, sdwanmetric2));
        sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);

        std::string linkmetricmap_key1(link1_info + "::" + sla_profile + "::" + traffic_type);
        std::string linkmetricmap_key2(link2_info + "::" + sla_profile + "::" + traffic_type);
        LOG(DEBUG,"UVE: link_metrics_*_traffic_type key1 :" << linkmetricmap_key1);
        LOG(DEBUG,"UVE: link_metrics_*_traffic_type key2 :" << linkmetricmap_key2);
        link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key1, sdwanmetric1));
        link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key2, sdwanmetric2));
        sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);

        // Map: tenant_metrics_diff_sla
        std::map<std::string, SDWANMetrics_diff> tenant_metrics_diff_sla;
        std::string tenantmetric_key1(location + "::" +
                                    sla_profile + "::" +
                                    traffic_type + "@" +
                                    traffic_destination_link1 + "@" +
                                    link_type_link1);
        std::string tenantmetric_key2(location + "::" +
                                    sla_profile + "::" +
                                    traffic_type + "@" +
                                    traffic_destination_link2 + "@" + 
                                    link_type_link2);
        LOG(DEBUG,"UVE: tenant_metrics_diff_sla key1 :" << tenantmetric_key1);
        LOG(DEBUG,"UVE: tenant_metrics_diff_sla key2 :" << tenantmetric_key2);
        tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key1, sdwanmetric1));
        tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key2, sdwanmetric2));
        sdwantenantmetricrecord.set_tenant_metrics_diff_sla(tenant_metrics_diff_sla);

        // Update maps for underlay links if needed
        /*
        if ((!(boost::equals(link1, underlay_link1))) ||
           (!(boost::equals(link2, underlay_link2)))) {
            if (!(boost::equals(link1, underlay_link1))) {
                std::string linkmap_key1(tt_app_dept_info + underlay_link1);
                std::string linkmetricmap_key1(underlay_link1 + "::" + sla_profile + "::" + traffic_type);
                LOG(DEBUG,"UVE: underlay app_metrics_diff_link key1 :" << linkmap_key1);
                LOG(DEBUG,"UVE: underlay link_metrics_*_traffic_type key1 :" << linkmetricmap_key1);
                app_metrics_diff_link.insert(std::make_pair(linkmap_key1, sdwanmetric1));
                link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key1, sdwanmetric1));
            }
            if ((!(boost::equals(link2, underlay_link2))) &&
               (!(boost::equals(underlay_link1, underlay_link2)))) {
                std::string linkmap_key2(tt_app_dept_info + underlay_link2);
                std::string linkmetricmap_key2(underlay_link2 + "::" + sla_profile + "::" + traffic_type);
                LOG(DEBUG,"UVE: underlay app_metrics_diff_link key2 :" << linkmap_key2);
                LOG(DEBUG,"UVE: underlay link_metrics_*_traffic_type key2 :" << linkmetricmap_key2);
                app_metrics_diff_link.insert(std::make_pair(linkmap_key2, sdwanmetric2));
                link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key2, sdwanmetric2));

            }
            sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);
            sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);
        }
        */
    }

    SDWANMetrics::Send(sdwanmetricrecord, "ObjectCPETable");
    SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");

    return;
}


double calculate_link_score(int64_t latency, int64_t packet_loss, int64_t jitter,
                           int64_t effective_latency_threshold, int64_t latency_factor,
                           int64_t jitter_factor, int64_t packet_loss_factor) {

    double effective_latency, r_factor, mos, latency_ms, jitter_ms;
    latency_ms = latency/1000;  // latency in milli secs
    jitter_ms = jitter/1000;    // jitter in milli secs

    // Setting the default values for coefficients
    if (effective_latency_threshold == 0)
        effective_latency_threshold = 160;
    if (latency_factor == 0)
        latency_factor = 100;
    if (jitter_factor == 0)
        jitter_factor = 200;
    if (packet_loss_factor == 0)
        packet_loss_factor = 250;

    LOG(DEBUG, "Link score calculation coefficients, effective_latency_threshold : "
                                                   << effective_latency_threshold
                                                   << ", latency_factor : " << latency_factor
                                                   << ", jitter_factor : " << jitter_factor
                                                   << ", packet_loss_factor : " 
                                                   << packet_loss_factor);

    // Step-1: Calculate EffectiveLatency = (AvgLatency + 2*AvgPositiveJitter + 10)
    effective_latency =
    ((latency_ms * (latency_factor/100.0)) + ((jitter_factor/100.0)*jitter_ms) + 10);
    // Step-2: Calculate Intermediate R-Value
    if (effective_latency < effective_latency_threshold) {
        r_factor = 93.2 - (effective_latency/40);
    }
    else {
        r_factor = 93.2 - (effective_latency - 120)/10;
    }
    // Step-3: Adjust R-Value for PacketLoss
    r_factor =  r_factor - (packet_loss * packet_loss_factor/100.0);
    // Step-4: Calculate MeanOpinionScore
    if (r_factor < 0 ){
        mos = 1;
    }
    else if ((r_factor > 0) && (r_factor < 100)) {
        mos = 1 + (0.035) * r_factor + (0.000007) * r_factor * (r_factor - 60) * (100 - r_factor);
    }
    else {
        mos = 4.5;
    }
    LOG(DEBUG, "Calculated Mean Opinion Score (link_score) for sla params is : " << mos);
    return (mos * 20);
}


void StructuredSyslogUVESummarizeAppQoePSMR(SyslogParser::syslog_m_t v, bool summarize_user) {
    SDWANMetricsRecord sdwanmetricrecord;
    SDWANTenantMetricsRecord sdwantenantmetricrecord;
    const std::string location(SyslogParser::GetMapVals(v, "location", "UNKNOWN"));
    const std::string tenant(SyslogParser::GetMapVals(v, "tenant", "UNKNOWN"));
    const std::string link(SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN"));
    const std::string sla_profile(SyslogParser::GetMapVals(v, "sla-profile", "UNKNOWN"));
    const std::string app_category(SyslogParser::GetMapVals(v, "app-category", "UNKNOWN"));
    const std::string department(SyslogParser::GetMapVals(v, "source-zone-name", "UNKNOWN"));
    const std::string device_id(SyslogParser::GetMapVals(v, "device", "UNKNOWN"));
    const std::string region(SyslogParser::GetMapVals(v, "region", "DEFAULT"));
    const std::string opco(SyslogParser::GetMapVals(v, "OPCO", "DEFAULT"));
    const std::string uvename = tenant + "::" + location + "::" + device_id;
    const std::string tenantuvename = region + "::" + opco + "::" + tenant;
    const std::string traffic_type(SyslogParser::GetMapVals(v, "active-probe-params", "UNKNOWN"));
    const std::string ip_dscp(SyslogParser::GetMapVals(v, "ip-dscp", "UNKNOWN"));
    const std::string dscp_alias_code(SyslogParser::GetMapVals(v, "dscp-alias-code", "UNKNOWN"));
    std::string nested_appname(SyslogParser::GetMapVals(v, "nested-application", "UNKNOWN"));
    std::string service_name(SyslogParser::GetMapVals(v, "service-name", "UNKNOWN"));
    std::string appname(SyslogParser::GetMapVals(v, "application", "UNKNOWN"));

    const std::string underlay_link = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
    const std::string link_type_link = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
    const std::string traffic_destination_link = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
    const std::string metadata_link = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));
    const std::string link_info = link + "@" + underlay_link
        + "@" + link_type_link + "@" + traffic_destination_link + "@" + metadata_link ;

    LOG(DEBUG,"UVE: dscp-alias-code: " << dscp_alias_code);
    LOG(DEBUG,"UVE: ip-dscp: " << ip_dscp);

    if (boost::iequals(nested_appname, "UNKNOWN") && boost::iequals(appname, "UNKNOWN")) {
        if (!(boost::iequals(service_name, "UNKNOWN")) && !(boost::iequals(service_name, "None"))) {
            nested_appname =  service_name;
            appname = service_name;
        }
    }

    // nested_appname@UNKNOWN, nested_appname@dscp_alias_code nested_appname@DSCP-dscp_value
    std::string dscp_key = "DSCP-" + dscp_alias_code;
    if (ip_dscp == "UNKNOWN") {
        dscp_key = "UNKNOWN";
    }
    else if (dscp_alias_code == "UNKNOWN") {
        dscp_key = "DSCP-" + ip_dscp;
    }

    const std::string nested_appname_with_alias_code = nested_appname + "@" + dscp_key;

    const std::string tt_app_dept_info = traffic_type + "(" + nested_appname_with_alias_code + ":" + appname
                                         + "/" + app_category +  ")" + "::" + department + "::";

    //username => syslog.username or syslog.source-address
    std::string username(SyslogParser::GetMapVals(v, "username", "UNKNOWN"));
    if (boost::iequals(username, "unknown")) {
        username = SyslogParser::GetMapVals(v, "source-address", "UNKNOWN");
    }
    sdwanmetricrecord.set_name(uvename);
    sdwantenantmetricrecord.set_name(tenantuvename);
    SDWANMetrics_dial sdwanmetric;
    int64_t pkt_loss = SyslogParser::GetMapVal(v, "pkt-loss", -1);
    int64_t rtt = SyslogParser::GetMapVal(v, "rtt", -1);
    int64_t rtt_jitter = SyslogParser::GetMapVal(v, "rtt-jitter", -1);
    int64_t egress_jitter = SyslogParser::GetMapVal(v, "egress-jitter", -1);
    int64_t ingress_jitter = SyslogParser::GetMapVal(v, "ingress-jitter", -1);

     /*
        Device sends high values for SLA parameters in syslog
        when probe is not successfull or for some reason
        device is not able to calculate SLA parameters.
        High values for -
        rtt -> 4294967295
        rtt_jitter -> 4294967295
        pkt_loss -> 255
        These high values should NOT be used for calculation and
        should be avoided.
    */
    if (rtt != -1 && rtt != 4294967295) {
        sdwanmetric.set_rtt(rtt);
    }
    if (rtt_jitter != -1 && rtt_jitter != 4294967295) {
        sdwanmetric.set_rtt_jitter(rtt_jitter);
    }
    if (egress_jitter != -1 && egress_jitter != 4294967295) {
        sdwanmetric.set_egress_jitter(egress_jitter);
    }
    if (ingress_jitter != -1 && ingress_jitter != 4294967295) {
        sdwanmetric.set_ingress_jitter(ingress_jitter);
    }
    if (pkt_loss != -1 && pkt_loss != 255) {
        // this check is added to correct the cases in which device sends
        // incorrect values containing loss% to be more than 100.
        if (pkt_loss > 100) {
            pkt_loss = 100;
        }
        sdwanmetric.set_pkt_loss(pkt_loss);
    }
    if ((rtt != -1) && (rtt != 4294967295) &&
        (rtt_jitter != -1) && (rtt_jitter != 4294967295) &&
        (pkt_loss != -1) && (pkt_loss != 255)) {
        sdwanmetric.set_score((int64_t)calculate_link_score(rtt/2, pkt_loss, rtt_jitter,
                             SyslogParser::GetMapVal(v, "effective-latency-threshold",0),
                             SyslogParser::GetMapVal(v, "latency-factor",0),
                             SyslogParser::GetMapVal(v, "jitter-factor",0),
                             SyslogParser::GetMapVal(v, "packet-loss-factor",0) ));
    }

    // Map:  app_metrics_dial_sla
    std::map<std::string, SDWANMetrics_dial> app_metrics_dial_sla;
    std::string slamap_key(tt_app_dept_info + sla_profile);
    LOG(DEBUG,"UVE: app_metrics_dial_sla key :" << slamap_key);
    app_metrics_dial_sla.insert(std::make_pair(slamap_key, sdwanmetric));
    sdwanmetricrecord.set_app_metrics_dial_sla(app_metrics_dial_sla);

    // Map:  app_metrics_dial_user
    if (summarize_user == true) {
        std::map<std::string, SDWANMetrics_dial> app_metrics_dial_user;
        std::string usermap_key(tt_app_dept_info + username);
        LOG(DEBUG,"UVE: app_metrics_dial_user key :" << usermap_key);
        app_metrics_dial_user.insert(std::make_pair(usermap_key, sdwanmetric));
        sdwanmetricrecord.set_app_metrics_dial_user(app_metrics_dial_user);
    }
    // Map:  app_metrics_dial_link
    std::map<std::string, SDWANMetrics_dial> app_metrics_dial_link;
    std::string linkmap_key(tt_app_dept_info + link_info);
    LOG(DEBUG,"UVE: app_metrics_dial_link key :" << linkmap_key);
    app_metrics_dial_link.insert(std::make_pair(linkmap_key, sdwanmetric));
    sdwanmetricrecord.set_app_metrics_dial_link(app_metrics_dial_link);

    // Map:  link_metrics_dial_traffic_type
    std::map<std::string, SDWANMetrics_dial> link_metrics_dial_traffic_type;
    std::string linkmetric_key(link_info + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: link_metrics_dial_traffic_type key :" << linkmetric_key);
    link_metrics_dial_traffic_type.insert(std::make_pair(linkmetric_key, sdwanmetric));
    sdwanmetricrecord.set_link_metrics_dial_traffic_type(link_metrics_dial_traffic_type);

    // Map: tenant_metrics_dial_sla
    std::map<std::string, SDWANMetrics_dial> tenant_metrics_dial_sla;
    std::string tenantmetric_key(location + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: tenant_metrics_dial_sla key :" << tenantmetric_key);
    tenant_metrics_dial_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric));
    sdwantenantmetricrecord.set_tenant_metrics_dial_sla(tenant_metrics_dial_sla);

    SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");
    SDWANMetrics::Send(sdwanmetricrecord, "ObjectCPETable");
    return;
}

void StructuredSyslogUVESummarizeAppQoeBPS(SyslogParser::syslog_m_t v, bool summarize_user) {
    SDWANMetricsRecord sdwanmetricrecord;
    SDWANTenantMetricsRecord sdwantenantmetricrecord;
    const std::string location(SyslogParser::GetMapVals(v, "location", "UNKNOWN"));
    const std::string tenant(SyslogParser::GetMapVals(v, "tenant", "UNKNOWN"));
    const std::string link(SyslogParser::GetMapVals(v, "previous-interface", "UNKNOWN"));
    const std::string sla_profile(SyslogParser::GetMapVals(v, "sla-profile", "UNKNOWN"));
    const std::string app_category(SyslogParser::GetMapVals(v, "app-category", "UNKNOWN"));
    const std::string department(SyslogParser::GetMapVals(v, "source-zone-name", "UNKNOWN"));
    const std::string device_id(SyslogParser::GetMapVals(v, "device", "UNKNOWN"));
    const std::string region(SyslogParser::GetMapVals(v, "region", "DEFAULT"));
    const std::string opco(SyslogParser::GetMapVals(v, "OPCO", "DEFAULT"));
    const std::string uvename = tenant + "::" + location + "::" + device_id;
    const std::string tenantuvename = region + "::" + opco + "::" + tenant;
    const std::string traffic_type(SyslogParser::GetMapVals(v, "active-probe-params", "UNKNOWN"));
    const std::string ip_dscp(SyslogParser::GetMapVals(v, "dscp-alias-code", "UNKNOWN"));
    const std::string dscp_alias_code(SyslogParser::GetMapVals(v, "ip-dscp", "UNKNOWN"));
    std::string nested_appname(SyslogParser::GetMapVals(v, "nested-application", "UNKNOWN"));
    std::string service_name(SyslogParser::GetMapVals(v, "service-name", "UNKNOWN"));
    std::string appname(SyslogParser::GetMapVals(v, "application", "UNKNOWN"));

    const std::string underlay_link = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
    const std::string link_type_link = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
    const std::string traffic_destination_link = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
    const std::string metadata_link = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));
    const std::string link_info = link + "@" + underlay_link
        + "@" + link_type_link + "@" + traffic_destination_link + "@" + metadata_link ;

    LOG(DEBUG,"UVE: dscp-alias-code: " << dscp_alias_code);
    LOG(DEBUG,"UVE: ip-dscp: " << ip_dscp);

    if (boost::iequals(nested_appname, "UNKNOWN") && boost::iequals(appname, "UNKNOWN")) {
        if (!(boost::iequals(service_name, "UNKNOWN")) && !(boost::iequals(service_name, "None"))) {
            nested_appname =  service_name;
            appname = service_name;
        }
    }

    // nested_appname@UNKNOWN, nested_appname@dscp_alias_code nested_appname@DSCP-dscp_value
    std::string dscp_key = "DSCP-" + dscp_alias_code;
    if (ip_dscp == "UNKNOWN") {
        dscp_key = "UNKNOWN";
    }
    else if (dscp_alias_code == "UNKNOWN") {
        dscp_key = "DSCP-" + ip_dscp;
    }

    const std::string nested_appname_with_alias_code = nested_appname + "@" + dscp_key;

    const std::string tt_app_dept_info = traffic_type + "(" + nested_appname_with_alias_code + ":" + appname
                                         + "/" + app_category +  ")" + "::" + department + "::";
    int64_t elapsed_time = SyslogParser::GetMapVal(v, "elapsed-time", 0);
    const std::string reason = SyslogParser::GetMapVals(v, "reason", "UNKNOWN");
    //username => syslog.username or syslog.source-address
    std::string username(SyslogParser::GetMapVals(v, "username", "UNKNOWN"));
    if (boost::iequals(username, "unknown")) {
        username = SyslogParser::GetMapVals(v, "source-address", "UNKNOWN");
    }
    sdwanmetricrecord.set_name(uvename);
    sdwantenantmetricrecord.set_name(tenantuvename);

    SDWANMetrics_diff sdwanmetric;
    if ((elapsed_time > 2) && (!(boost::iequals(reason, "session close")))){
        sdwanmetric.set_session_switch_count(1);
    }

    // Map: app_metrics_diff_sla
    std::map<std::string, SDWANMetrics_diff> app_metrics_diff_sla;
    std::string slamap_key(tt_app_dept_info + sla_profile);
    LOG(DEBUG,"UVE: app_metrics_diff_sla key :" << slamap_key);
    app_metrics_diff_sla.insert(std::make_pair(slamap_key, sdwanmetric));
    sdwanmetricrecord.set_app_metrics_diff_sla(app_metrics_diff_sla);

    // Map: app_metrics_diff_user
    if (summarize_user == true) {
        std::map<std::string, SDWANMetrics_diff> app_metrics_diff_user;
        std::string usermap_key(tt_app_dept_info + username);
        LOG(DEBUG,"UVE: app_metrics_diff_user key :" << usermap_key);
        app_metrics_diff_user.insert(std::make_pair(usermap_key, sdwanmetric));
        sdwanmetricrecord.set_app_metrics_diff_user(app_metrics_diff_user);
    }

    // Map: app_metrics_diff_link
    std::map<std::string, SDWANMetrics_diff> app_metrics_diff_link;
    std::string linkmap_key(tt_app_dept_info + link_info);
    LOG(DEBUG,"UVE: app_metrics_diff_link key :" << linkmap_key);
    app_metrics_diff_link.insert(std::make_pair(linkmap_key, sdwanmetric));
    sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);

    // Map: tenant_metrics_diff_sla
    std::map<std::string, SDWANMetrics_diff> tenant_metrics_diff_sla;
    std::string tenantmetric_key(location + "::" + sla_profile + "::" + traffic_type + "@" + traffic_destination_link + "@" + link_type_link);
    LOG(DEBUG,"UVE: tenant_metrics_diff_sla key :" << tenantmetric_key);
    tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric));
    sdwantenantmetricrecord.set_tenant_metrics_diff_sla(tenant_metrics_diff_sla);

    // Map: link_metrics_diff_traffic_type
    std::map<std::string, SDWANMetrics_diff> link_metrics_diff_traffic_type;
    std::string linkmetricmap_key(link_info + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: link_metrics_diff_traffic_type key :" << linkmetricmap_key);
    link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key, sdwanmetric));
    sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);

    SDWANMetrics::Send(sdwanmetricrecord, "ObjectCPETable");
    SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");

    return;
}

void StructuredSyslogUVESummarizeAppQoeSMV(SyslogParser::syslog_m_t v, bool summarize_user) {
    SDWANMetricsRecord sdwanmetricrecord;
    SDWANTenantMetricsRecord sdwantenantmetricrecord;
    const std::string location(SyslogParser::GetMapVals(v, "location", "UNKNOWN"));
    const std::string tenant(SyslogParser::GetMapVals(v, "tenant", "UNKNOWN"));
    const std::string link(SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN"));
    const std::string sla_profile(SyslogParser::GetMapVals(v, "sla-profile", "UNKNOWN"));
    const std::string app_category(SyslogParser::GetMapVals(v, "app-category", "UNKNOWN"));
    const std::string department(SyslogParser::GetMapVals(v, "source-zone-name", "UNKNOWN"));
    const std::string device_id(SyslogParser::GetMapVals(v, "device", "UNKNOWN"));
    const std::string region(SyslogParser::GetMapVals(v, "region", "DEFAULT"));
    const std::string opco(SyslogParser::GetMapVals(v, "OPCO", "DEFAULT"));
    const std::string uvename = tenant + "::" + location + "::" + device_id;
    const std::string tenantuvename = region + "::" + opco + "::" + tenant;
    const std::string traffic_type(SyslogParser::GetMapVals(v, "active-probe-params", "UNKNOWN"));
    const std::string ip_dscp(SyslogParser::GetMapVals(v, "ip-dscp", "UNKNOWN"));
    const std::string dscp_alias_code(SyslogParser::GetMapVals(v, "dscp-alias-code", "UNKNOWN"));
    std::string nested_appname(SyslogParser::GetMapVals(v, "nested-application", "UNKNOWN"));
    std::string appname(SyslogParser::GetMapVals(v, "application", "UNKNOWN"));
    std::string service_name(SyslogParser::GetMapVals(v, "service-name", "UNKNOWN"));

    const std::string underlay_link = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
    const std::string link_type_link = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
    const std::string traffic_destination_link = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
    const std::string metadata_link = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));
    const std::string link_info = link + "@" + underlay_link
        + "@" + link_type_link + "@" + traffic_destination_link + "@" + metadata_link ;


    LOG(DEBUG,"UVE: dscp-alias-code: " << dscp_alias_code);
    LOG(DEBUG,"UVE: ip-dscp: " << ip_dscp);

    if (boost::iequals(nested_appname, "UNKNOWN") && boost::iequals(appname, "UNKNOWN")) {
        if (!(boost::iequals(service_name, "UNKNOWN")) && !(boost::iequals(service_name, "None"))) {
            nested_appname =  service_name;
            appname = service_name;
        }
    }

    // nested_appname@UNKNOWN, nested_appname@dscp_alias_code nested_appname@DSCP-dscp_value
    std::string dscp_key = "DSCP-" + dscp_alias_code;
    if (ip_dscp == "UNKNOWN") {
        dscp_key = "UNKNOWN";
    }
    else if (dscp_alias_code == "UNKNOWN") {
        dscp_key = "DSCP-" + ip_dscp;
    }

    const std::string nested_appname_with_alias_code = nested_appname + "@" + dscp_key;
    const std::string tt_app_dept_info = traffic_type + "(" + nested_appname_with_alias_code + ":" + appname
                                         + "/" + app_category +  ")" + "::" + department + "::";

    //username => syslog.username or syslog.source-address
    std::string username(SyslogParser::GetMapVals(v, "username", "UNKNOWN"));
    if (boost::iequals(username, "unknown")) {
        username = SyslogParser::GetMapVals(v, "source-address", "UNKNOWN");
    }
    sdwanmetricrecord.set_name(uvename);
    sdwantenantmetricrecord.set_name(tenantuvename);

    SDWANMetrics_diff sdwanmetric;
    //SDWANMetrics_dial sdwanmetric_dial;
    //int64_t sampling_percentage = SyslogParser::GetMapVal(v, "sampling-percentage", -1);
    //if (sampling_percentage != -1) {
    //    sdwanmetric_dial.set_sampling_percentage(sampling_percentage);
    //}
    int64_t violation_reason = SyslogParser::GetMapVal(v, "violation-reason", -1);
    if (violation_reason > 0) {
        sdwanmetric.set_sla_violation_count(1);
        if (SyslogParser::GetMapVal(v, "jitter-violation-count", 0) != 0) {
            sdwanmetric.set_jitter_violation_count(1);
        }
        if (SyslogParser::GetMapVal(v, "rtt-violation-count", 0) != 0) {
            sdwanmetric.set_rtt_violation_count(1);
        }
        if (SyslogParser::GetMapVal(v, "pkt-loss-violation-count", 0) != 0) {
            sdwanmetric.set_pkt_loss_violation_count(1);
        }
    }
    else if (violation_reason == 0) {
        sdwanmetric.set_sla_violation_duration(SyslogParser::GetMapVal(v, "violation-duration", 0));
    }
    else {
        return;
    }

    // Map: app_metrics_*_sla
    std::map<std::string, SDWANMetrics_diff> app_metrics_diff_sla;
    std::map<std::string, SDWANMetrics_dial> app_metrics_dial_sla;
    std::string slamap_key(tt_app_dept_info + sla_profile);
    LOG(DEBUG,"UVE: app_metrics_*_sla key :" << slamap_key);
    app_metrics_diff_sla.insert(std::make_pair(slamap_key, sdwanmetric));
    //app_metrics_dial_sla.insert(std::make_pair(slamap_key, sdwanmetric_dial));
    sdwanmetricrecord.set_app_metrics_diff_sla(app_metrics_diff_sla);
    //sdwanmetricrecord.set_app_metrics_dial_sla(app_metrics_dial_sla);

    // Map: app_metrics_*_user
    if (summarize_user == true) {
        std::map<std::string, SDWANMetrics_diff> app_metrics_diff_user;
        std::map<std::string, SDWANMetrics_dial> app_metrics_dial_user;
        std::string usermap_key(tt_app_dept_info + username);
        LOG(DEBUG,"UVE: app_metrics_*_user key :" << usermap_key);
        app_metrics_diff_user.insert(std::make_pair(usermap_key, sdwanmetric));
        //app_metrics_dial_user.insert(std::make_pair(usermap_key, sdwanmetric_dial));
        sdwanmetricrecord.set_app_metrics_diff_user(app_metrics_diff_user);
        sdwanmetricrecord.set_app_metrics_dial_user(app_metrics_dial_user);
    }
    // Map: app_metrics_*_link
    std::map<std::string, SDWANMetrics_diff> app_metrics_diff_link;
    //std::map<std::string, SDWANMetrics_dial> app_metrics_dial_link;
    std::string linkmap_key(tt_app_dept_info + link_info);
    LOG(DEBUG,"UVE: app_metrics_*_link key :" << linkmap_key);
    app_metrics_diff_link.insert(std::make_pair(linkmap_key, sdwanmetric));
    //app_metrics_dial_link.insert(std::make_pair(linkmap_key, sdwanmetric_dial));
    sdwanmetricrecord.set_app_metrics_diff_link(app_metrics_diff_link);
    //sdwanmetricrecord.set_app_metrics_dial_link(app_metrics_dial_link);

    // Map: tenant_metrics_*_sla
    std::map<std::string, SDWANMetrics_diff> tenant_metrics_diff_sla;
    std::map<std::string, SDWANMetrics_dial> tenant_metrics_dial_sla;
    std::string tenantmetric_key(location + "::" + sla_profile + "::" + traffic_type + "@" + traffic_destination_link + "@" + link_type_link);
    LOG(DEBUG,"UVE: tenant_metrics_*_sla key :" << tenantmetric_key);
    tenant_metrics_diff_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric));
    //tenant_metrics_dial_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric_dial));
    sdwantenantmetricrecord.set_tenant_metrics_diff_sla(tenant_metrics_diff_sla);
    //sdwantenantmetricrecord.set_tenant_metrics_dial_sla(tenant_metrics_dial_sla);

    // Map: link_metrics_*_traffic_type
    std::map<std::string, SDWANMetrics_diff> link_metrics_diff_traffic_type;
    std::map<std::string, SDWANMetrics_dial> link_metrics_dial_traffic_type;
    std::string linkmetricmap_key(link_info + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: link_metrics_*_traffic_type key :" << linkmetricmap_key);
    link_metrics_diff_traffic_type.insert(std::make_pair(linkmetricmap_key, sdwanmetric));
    //link_metrics_dial_traffic_type.insert(std::make_pair(linkmetricmap_key, sdwanmetric_dial));
    sdwanmetricrecord.set_link_metrics_diff_traffic_type(link_metrics_diff_traffic_type);
    //sdwanmetricrecord.set_link_metrics_dial_traffic_type(link_metrics_dial_traffic_type);

    SDWANMetrics::Send(sdwanmetricrecord, "ObjectCPETable");
    SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");

    return;
}

void StructuredSyslogUVESummarizeAppQoeASMR(SyslogParser::syslog_m_t v, bool summarize_user) {
    SDWANMetricsRecord sdwanmetricrecord;
    SDWANTenantMetricsRecord sdwantenantmetricrecord;
    const std::string location(SyslogParser::GetMapVals(v, "location", "UNKNOWN"));
    const std::string tenant(SyslogParser::GetMapVals(v, "tenant", "UNKNOWN"));
    const std::string link(SyslogParser::GetMapVals(v, "destination-interface-name", "UNKNOWN"));
    const std::string sla_profile(SyslogParser::GetMapVals(v, "sla-profile", "UNKNOWN"));
    const std::string device_id(SyslogParser::GetMapVals(v, "device", "UNKNOWN"));
    const std::string region(SyslogParser::GetMapVals(v, "region", "DEFAULT"));
    const std::string opco(SyslogParser::GetMapVals(v, "OPCO", "DEFAULT"));
    const std::string uvename = tenant + "::" + location + "::" + device_id;
    const std::string tenantuvename = region + "::" + opco + "::" + tenant;
    const std::string traffic_type(SyslogParser::GetMapVals(v, "active-probe-params", "UNKNOWN"));
    const std::string underlay_link = (SyslogParser::GetMapVals(v, "underlay-destination-interface-name", "UNKNOWN"));
    const std::string link_type_link = (SyslogParser::GetMapVals(v, "link-type-destination-interface-name", "UNKNOWN"));
    const std::string traffic_destination_link = (SyslogParser::GetMapVals(v, "traffic-destination-destination-interface-name", "UNKNOWN"));
    const std::string metadata_link = (SyslogParser::GetMapVals(v, "metadata-destination-interface-name", "UNKNOWN"));
    const std::string link_info = link + "@" + underlay_link
        + "@" + link_type_link + "@" + traffic_destination_link + "@" + metadata_link ;

    // DSCP value is not collected for ASMR syslog because we dont display on the basis of APP here.

    sdwanmetricrecord.set_name(uvename);
    sdwantenantmetricrecord.set_name(tenantuvename);
    SDWANMetrics_dial sdwanmetric;
    int64_t pkt_loss = SyslogParser::GetMapVal(v, "pkt-loss", -1);
    int64_t rtt = SyslogParser::GetMapVal(v, "rtt", -1);
    int64_t rtt_jitter = SyslogParser::GetMapVal(v, "rtt-jitter", -1);
    int64_t egress_jitter = SyslogParser::GetMapVal(v, "egress-jitter", -1);
    int64_t ingress_jitter = SyslogParser::GetMapVal(v, "ingress-jitter", -1);
    /*
        Device sends high values for SLA parameters in syslog
        when probe is not successfull or for some reason
        device is not able to calculate SLA parameters.
        High values for -
        rtt -> 4294967295
        rtt_jitter -> 4294967295
        pkt_loss -> 255
        These high values should NOT be used for calculation and
        should be avoided.
    */
    if (rtt != -1 && rtt != 4294967295) {
        sdwanmetric.set_rtt(rtt);
    }
    if (rtt_jitter != -1 && rtt_jitter != 4294967295) {
        sdwanmetric.set_rtt_jitter(rtt_jitter);
    }
    if (egress_jitter != -1 && egress_jitter != 4294967295) {
        sdwanmetric.set_egress_jitter(egress_jitter);
    }
    if (ingress_jitter != -1 && ingress_jitter != 4294967295) {
        sdwanmetric.set_ingress_jitter(ingress_jitter);
    }
    if (pkt_loss != -1 && pkt_loss != 255) {
        // this check is added to correct the cases in which device sends
        // incorrect values containing loss% to be more than 100.
        if (pkt_loss > 100) {
            pkt_loss = 100;
        }
        sdwanmetric.set_pkt_loss(pkt_loss);
    }
    if ((rtt != -1) && (rtt != 4294967295) &&
        (rtt_jitter != -1) && (rtt_jitter != 4294967295) &&
        (pkt_loss != -1) && (pkt_loss != 255)) {
        sdwanmetric.set_score((int64_t)calculate_link_score(rtt/2, pkt_loss, rtt_jitter,
                             SyslogParser::GetMapVal(v, "effective-latency-threshold",0),
                             SyslogParser::GetMapVal(v, "latency-factor",0),
                             SyslogParser::GetMapVal(v, "jitter-factor",0),
                             SyslogParser::GetMapVal(v, "packet-loss-factor",0) ));
    }
    // Map:  link_metrics_dial_traffic_type
    std::map<std::string, SDWANMetrics_dial> link_metrics_dial_traffic_type;
    std::string linkmap_key(link_info + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: link_metrics_dial_traffic_type key :" << linkmap_key);
    link_metrics_dial_traffic_type.insert(std::make_pair(linkmap_key, sdwanmetric));
    sdwanmetricrecord.set_link_metrics_dial_traffic_type(link_metrics_dial_traffic_type);

    // Map: tenant_metrics_dial_sla
    std::map<std::string, SDWANMetrics_dial> tenant_metrics_dial_sla;
    std::string tenantmetric_key(location + "::" + sla_profile + "::" + traffic_type);
    LOG(DEBUG,"UVE: tenant_metrics_dial_sla key :" << tenantmetric_key);
    tenant_metrics_dial_sla.insert(std::make_pair(tenantmetric_key, sdwanmetric));
    sdwantenantmetricrecord.set_tenant_metrics_dial_sla(tenant_metrics_dial_sla);

    SDWANMetrics::Send(sdwanmetricrecord, "ObjectCPETable");
    SDWANTenantMetrics::Send(sdwantenantmetricrecord, "ObjectCPETable");
    return;
}

void StructuredSyslogUVESummarize(SyslogParser::syslog_m_t v, bool summarize_user, StructuredSyslogConfig *config_obj) {
    const std::string tag(SyslogParser::GetMapVals(v, "tag", "UNKNOWN"));
    LOG(DEBUG,"UVE: Summarizing " << tag << " as UVE with flag summarize_user:" << summarize_user);
    if (boost::equals(tag, "APPTRACK_SESSION_CLOSE")) {
        StructuredSyslogUVESummarizeData(v, summarize_user, config_obj);
    }
    else if (boost::equals(tag, "APPTRACK_SESSION_VOL_UPDATE")) {
        StructuredSyslogUVESummarizeData(v, summarize_user, config_obj);
    }
    else if (boost::equals(tag, "RT_FLOW_NEXTHOP_CHANGE")) {
        StructuredSyslogUVESummarizeData(v, summarize_user, config_obj);
    }
    else if (boost::equals(tag, "APPQOE_BEST_PATH_SELECTED")) {
        StructuredSyslogUVESummarizeAppQoeBPS(v, summarize_user);
    }
    else if (boost::equals(tag, "APPQOE_PASSIVE_SLA_METRIC_REPORT") ||
             boost::equals(tag, "APPQOE_APP_PASSIVE_SLA_METRIC_REPORT")) {
        StructuredSyslogUVESummarizeAppQoePSMR(v, summarize_user);
    }
    else if (boost::equals(tag, "APPQOE_ACTIVE_SLA_METRIC_REPORT")) {
        StructuredSyslogUVESummarizeAppQoeASMR(v, summarize_user);
    }
    else if (boost::equals(tag, "APPQOE_SLA_METRIC_VIOLATION")) {
        StructuredSyslogUVESummarizeAppQoeSMV(v, summarize_user);
    }
}

void StructuredSyslogDecorate (SyslogParser::syslog_m_t &v, StructuredSyslogConfig *config_obj,
                               boost::shared_ptr<std::string> msg, std::vector<std::string> int_fields) {

    int64_t from_client = SyslogParser::GetMapVal(v, "bytes-from-client", 0);
    int64_t from_server = SyslogParser::GetMapVal(v, "bytes-from-server", 0);
    size_t prev_pos = 0;
    v.insert(std::pair<std::string, SyslogParser::Holder>("total-bytes",
    SyslogParser::Holder("total-bytes", (from_client + from_server))));
    std::stringstream total_bytes;
    total_bytes << (from_client + from_server);
    prev_pos = DecorateMsg(msg, "total-bytes", total_bytes.str(), prev_pos);
    v.insert(std::pair<std::string, SyslogParser::Holder>("tenant",
    SyslogParser::Holder("tenant", "UNKNOWN")));
    v.insert(std::pair<std::string, SyslogParser::Holder>("location",
    SyslogParser::Holder("location", "UNKNOWN")));
    v.insert(std::pair<std::string, SyslogParser::Holder>("device",
    SyslogParser::Holder("device", "UNKNOWN")));
    /* get sla-profile from rule-name or sla-rule */
    /* rule-name="r_apbr_d_ENG_p_Intent-1_s_sla-profile1" */
    std::string sla_profile = SyslogParser::GetMapVals(v, "sla-rule", "");
    if (sla_profile.empty()) {
        std::string rn = SyslogParser::GetMapVals(v, "rule-name", "");
        if (!rn.empty()) {
            size_t start = rn.find_last_of('_');
            if (start != string::npos) {
                sla_profile = rn.substr(start+1);
            }
            else {
                sla_profile = "DEFAULT";
            }
        }
        else {
            sla_profile = "DEFAULT";
        }
    }
    v.insert(std::pair<std::string, SyslogParser::Holder>("sla-profile",
    SyslogParser::Holder("sla-profile", sla_profile)));
    prev_pos = DecorateMsg(msg, "sla-profile", sla_profile, prev_pos);

    if (config_obj != NULL) {
        std::string hn = SyslogParser::GetMapVals(v, "hostname", "");
        boost::shared_ptr<HostnameRecord> hr = config_obj->GetHostnameRecord(hn);
        if (hr != NULL) {
            LOG(DEBUG, "StructuredSyslogDecorate hostname record: " << hn);
            const std::string tenant = hr->tenant();
            if (!tenant.empty()) {
                v.erase("tenant");
                v.insert(std::pair<std::string, SyslogParser::Holder>("tenant",
                SyslogParser::Holder("tenant", tenant)));
                prev_pos = DecorateMsg(msg, "tenant", tenant, prev_pos);
            }
            const std::string location = hr->location();
            if (!location.empty()) {
                v.erase("location");
                v.insert(std::pair<std::string, SyslogParser::Holder>("location",
                SyslogParser::Holder("location", location)));
                prev_pos = DecorateMsg(msg, "location", location, prev_pos);
            }
            const std::string device = hr->device();
            if (!device.empty()) {
                v.erase("device");
                v.insert(std::pair<std::string, SyslogParser::Holder>("device",
                SyslogParser::Holder("device", device)));
                prev_pos = DecorateMsg(msg, "device", device, prev_pos);
            }
            const std::string hostname_tags = hr->tags();
            if (!hostname_tags.empty()) {
                ParseStructuredPart(&v, hostname_tags, int_fields, msg);
            }
            std::map< std::string, std::string > linkmap = hr->linkmap();
            std::string links[4] = { "destination-interface-name", "last-incoming-interface-name",
                                  "uplink-incoming-interface-name","last-destination-interface-name"};
            for (int i = 0; i < 4; i++) {
                std::string overlay_link(SyslogParser::GetMapVals(v, links[i], ""));
                if (!overlay_link.empty() && !(boost::equals(overlay_link,"N/A"))) {
                    std::map< std::string, std::string >::iterator it = linkmap.find(overlay_link);
                    if (it  != linkmap.end()) {
                        // linkmap value => underlay + "@" + link_type + "@" + traffic_destination + "@" + link_metadata ;
                        std::vector<std::string> link_data;
                        boost::split(link_data, it->second, boost::is_any_of("@"), boost::token_compress_on);
                        LOG(DEBUG, "StructuredSyslogDecorate: linkmap for " << links[i] << " : "
                            << overlay_link << " underlay " << link_data[0]);
                        LOG(DEBUG, "StructuredSyslogDecorate: linkmap for " << links[i] << " : "
                            << overlay_link << " link type " << link_data[1]);
                        LOG(DEBUG, "StructuredSyslogDecorate: linkmap for " << links[i] << " : "
                            << overlay_link << " traffic destination " << link_data[2]);
                        LOG(DEBUG, "StructuredSyslogDecorate: linkmap for " << links[i] << " : "
                            << overlay_link << " link metadata " << link_data[3]);

                        std::string underlay_link_name = "underlay-" + links[i];
                        v.insert(std::pair<std::string, SyslogParser::Holder>(underlay_link_name,
                              SyslogParser::Holder(underlay_link_name, link_data[0])));
                        std::string link_type = "link-type-" + links[i];
                        v.insert(std::pair<std::string, SyslogParser::Holder>(link_type,
                              SyslogParser::Holder(link_type, link_data[1])));
                        std::string traffic_destination = "traffic-destination-" + links[i];
                        v.insert(std::pair<std::string, SyslogParser::Holder>(traffic_destination,
                              SyslogParser::Holder(traffic_destination, link_data[2])));
                        std::string link_metadata = "metadata-" + links[i];
                        v.insert(std::pair<std::string, SyslogParser::Holder>(link_metadata,
                              SyslogParser::Holder(link_metadata, link_data[3])));
                    }
                }
            }

            //const std::string tenant_record_default_tenant = "DEFAULT";
            boost::shared_ptr<TenantRecord> tr = config_obj->GetTenantRecord(tenant);
            LOG(DEBUG, "StructuredSyslogDecorate: Processing Tenant Record !!");
            // if (tr == NULL) {
            //     tr = config_obj->GetTenantRecord(tenant_record_default_tenant);
            //     LOG(DEBUG, "StructuredSyslogDecorate: Tenant \"" << tenant << "\" not found. Reading \"DEFAULT\" Tenant Record !!");
            // }
            if (tr != NULL) {
                const std::string tenantaddr = tr->tenantaddr();
                if (!tenantaddr.empty()){
                    v.insert(std::pair<std::string, SyslogParser::Holder>("tenantaddr",
                        SyslogParser::Holder("tenantaddr", tenantaddr)));
                }

                const std::string tenant_tags = tr->tags();
                if (!tenant_tags.empty()) {
                    ParseStructuredPart(&v, tenant_tags, int_fields, msg);
                }

                //check if the syslog contains dscp-value(or ip-dscp) then find the corresponding alias-code 
                std::string dscp_value = SyslogParser::GetMapVals(v, "dscp-value", "");
                std::string ip_dscp = SyslogParser::GetMapVals(v, "ip-dscp", "");
                if (dscp_value.empty()) {
                    LOG(DEBUG, "StructuredSyslogDecorate: \"dscp-value\" field not found in syslog, reading \"ip-dscp\" instead ...");
                    dscp_value = ip_dscp;
                }
                if (!dscp_value.empty()) {
                    LOG(DEBUG, "StructuredSyslogDecorate: Finding alias-code for dscp-value " << dscp_value);
                    //find the internet protocol (IP) version for destination-address
                    const std::string destination_address (SyslogParser::GetMapVals(v, "destination-address", "UNKNOWN"));
                    int ip_version = 4;
                    if (destination_address != "UNKNOWN") {
                        ip_version = config_obj->get_ip_version (destination_address);
                    }
                    LOG(DEBUG, "StructuredSyslogDecorate: IP version for destination-address " << destination_address << " found to be IPv" << ip_version);
                    if (ip_version == 4) {
                        std::map< std::string, std::string > dscpmap_ipv4 = tr->dscpmap_ipv4();  
                        std::map< std::string, std::string >::iterator it = dscpmap_ipv4.find(dscp_value);
                        if (it != dscpmap_ipv4.end()) {
                            LOG(DEBUG, "StructuredSyslogDecorate: alias-code for dscp-value: " << dscp_value << " is "
                                << it->second );
                            v.insert(std::pair<std::string, SyslogParser::Holder>("dscp-alias-code",
                                  SyslogParser::Holder("dscp-alias-code", it->second)));
                        }
                        else {
                            LOG(DEBUG, "StructuredSyslogDecorate: alias-code NOT found for dscp-value: " << dscp_value);
                        }
                    }
                    else if (ip_version == 6) {
                        std::map< std::string, std::string > dscpmap_ipv6 = tr->dscpmap_ipv6();  
                        std::map< std::string, std::string >::iterator it = dscpmap_ipv6.find(dscp_value);
                        if (it != dscpmap_ipv6.end()) {
                            LOG(DEBUG, "StructuredSyslogDecorate: alias-code for dscp-value: " << dscp_value << " is "
                                << it->second );
                            v.insert(std::pair<std::string, SyslogParser::Holder>("dscp-alias-code",
                                  SyslogParser::Holder("dscp-alias-code", it->second)));
                        }
                        else {
                            LOG(DEBUG, "StructuredSyslogDecorate: alias-code NOT found for dscp-value: " << dscp_value);
                        }
                    }
                    else {
                        LOG(ERROR, "StructuredSyslogDecorate: destination-address IP does not have any valid version. Skipping search for dscp-alias-code!");
                    }
                }
            }
            else {
                LOG(DEBUG, "StructuredSyslogDecorate: Tenant Record not found for: " << tenant);
            }


            /*
            std::string an = SyslogParser::GetMapVals(v, "nested-application", "unknown");
            if (boost::iequals(an, "unknown")) {
                an = SyslogParser::GetMapVals(v, "application", "unknown");
            }
            size_t found = an.find_first_of(':');
            while (found != string::npos) {
                an[found] = '/';
                found = an.find_first_of(':', found+1);
            }
            v.insert(std::pair<std::string, SyslogParser::Holder>("app-category",
            SyslogParser::Holder("app-category", "UNKNOWN")));
            v.insert(std::pair<std::string, SyslogParser::Holder>("app-subcategory",
            SyslogParser::Holder("app-subcategory", "UNKNOWN")));
            v.insert(std::pair<std::string, SyslogParser::Holder>("app-groups",
            SyslogParser::Holder("app-groups", "UNKNOWN")));
            v.insert(std::pair<std::string, SyslogParser::Holder>("app-risk",
            SyslogParser::Holder("app-risk", "UNKNOWN")));

            boost::shared_ptr<ApplicationRecord> ar;
            ar = config_obj->GetApplicationRecord(an);
            if (ar == NULL) {
                ar = config_obj->GetApplicationRecord("*");
            }
            if (ar != NULL) {
                LOG(DEBUG, "StructuredSyslogDecorate application record: " << an);
                const std::string app_category = ar->app_category();
                if (!app_category.empty()) {
                    v.erase("app-category");
                    v.insert(std::pair<std::string, SyslogParser::Holder>("app-category",
                    SyslogParser::Holder("app-category", app_category)));
                    prev_pos = DecorateMsg(msg, "app-category", app_category, prev_pos);
                }
                const std::string app_subcategory = ar->app_subcategory();
                if (!app_subcategory.empty()) {
                    v.erase("app-subcategory");
                    v.insert(std::pair<std::string, SyslogParser::Holder>("app-subcategory",
                    SyslogParser::Holder("app-subcategory", app_subcategory)));
                    prev_pos = DecorateMsg(msg, "app-subcategory", app_subcategory, prev_pos);
                }
                const std::string app_groups = ar->app_groups();
                if (!app_groups.empty()) {
                    v.erase("app-groups");
                    v.insert(std::pair<std::string, SyslogParser::Holder>("app-groups",
                    SyslogParser::Holder("app-groups", app_groups)));
                    prev_pos = DecorateMsg(msg, "app-groups", app_groups, prev_pos);
                }
                const std::string app_risk = ar->app_risk();
                if (!app_risk.empty()) {
                    v.erase("app-risk");
                    v.insert(std::pair<std::string, SyslogParser::Holder>("app-risk",
                    SyslogParser::Holder("app-risk", app_risk)));
                    prev_pos = DecorateMsg(msg, "app-risk", app_risk, prev_pos);
                }
                const std::string app_service_tags = ar->app_service_tags();
                if (!app_service_tags.empty()) {
                    ParseStructuredPart(&v, app_service_tags, int_fields, msg);
                }
            } else {
                LOG(INFO, "StructuredSyslogDecorate: Application Record not found for: " << an);
            }
            */
            std::string sla_rec_name = tenant + "/" + device + "/" + sla_profile;
            boost::shared_ptr<SlaProfileRecord> sla_rec;
            sla_rec = config_obj->GetSlaProfileRecord(sla_rec_name);
            if (sla_rec != NULL) {
                LOG(DEBUG, "StructuredSyslogDecorate sla-profile record: " << sla_rec_name);
                const std::string sla_params = sla_rec->sla_params();
                if (!sla_params.empty()) {
                    ParseStructuredPart(&v, sla_params, int_fields, msg);
                }
            }
        }
        else {
            LOG(DEBUG, "StructuredSyslogDecorate: Hostname Record not found for: " << hn);
        }
    }
}


/* -------Structured syslog parsing.
   -------For newly received tcp message on port 3514(structured-syslog port),

1. Check if there is old buffer from previous message (start ptr = 0)
    a. If yes,
    then append newly received tcp message to old buffer, and take forward
    appended message for further processing, otherwise take forward just
    the received message

2. Identifying the start and end of the message, ( Len = received buffer length
    or the appended buffer + received buffer length)
    a. Find the first space (" ")
    from the start, and take up the word from the start (pointer) to the space
    for parsing prepended syslog message length.
    b. If message length conversion doesn't turn up to be 0,
        - then end pointer is pointed to message length + no. of
          bytes after space and start pointer to first char after space.
    c. But if message length conversion is 0, determination of start and end of
    syslog message is done by parsing each byte/char of the message till either
    the start and end are found or we run our of Len.
        - The position of char '<' is taken as start pointer and
          char ']' is taken as end pointer.

3. If end char of structured syslog ']' is not found upon parsing the complete
received message, the message from the start till the end of received message is
put into session buffer anticipating the remaining part of structured syslog in
next tcp buffer which will be appended to session buffer as pointedin step 1.
The necessary condition here is that the end pointer index should be either
greater than the received tcp buffer length (len, in this case we will directly
put the message into buffer) or it should point to received tcp buffer length
(len), in this case will check for last element to have end delimiter ']'. check
for following condition below in code for step 3.  (((end > len) || ((end ==
len) && (*(p + end - 1) != ']') )) && (sess_buf != NULL))

4. If the above step (step 3) holds true, i.e. complete syslog message remains
to be be received, the process follows again from step 1. Else next step (step
5) follows for parsing the identified structured  syslog message.

5. The identified structured syslog message is first parsed for syslog part of
message and then structured part of syslog.

6. Upon parsing of syslog message in step 5, check for the remaining message in
received tcp buffer (multiple syslog messages can be in single buffer) and shift
start and end pointers as follows below.
    a. Shift the start pointer to 1 + end pointer of previously parsed message,
    if parsing the syslog in step 5 is unsuccessful.
    b. Shift the start pointer to previous start pointer + message length of
    previously parsed message, if parsing the syslog in step 5 is successful.

7. Check if start pointer index is equal to received tcp buffer length (meaning
the complete received tcp buffer is parsed), otherwise flow jumps to step 2 with
new (shifted) start pointer till we parse complete received tcp buffer. */


bool ProcessStructuredSyslog(const uint8_t *data, size_t len,
    const boost::asio::ip::address remote_address,
    StatWalker::StatTableInsertFn stat_db_callback, StructuredSyslogConfig *config_obj,
    boost::shared_ptr<StructuredSyslogForwarder> forwarder, boost::shared_ptr<std::string> sess_buf) {
  boost::system::error_code ec;
  const std::string ip(remote_address.to_string(ec));
  const uint8_t *p;
  std::string full_log;

  size_t end, len_end, start = 0;
  bool r;

  while (!*(data + len - 1))
      --len;
  LOG(DEBUG, "full structured_syslog: " << std::string(data + start, data + len) << " len: " << len);
  if (sess_buf != NULL) {
    full_log = *sess_buf + std::string(data + start, data + len);
    p = reinterpret_cast<const uint8_t*>(full_log.data());
    len += sess_buf->length();
    LOG(DEBUG, "structured_syslog sess_buf + new buf: " << full_log << " len: " <<  len);
    sess_buf->clear();
  } else {
    p = data;
  }
  
  do {
      SyslogParser::syslog_m_t v;
      end = start + 1;
      len_end = start + 1;
      while ((*(p + len_end - 1) != ' ') && (len_end < len))
        ++len_end;

      /* use prepended msg len to find the end of msg, if it exists */
      if (len_end != len) {
          bool is_msg_len = true;
          int mlen_idx = start;
          long int mlen = 0;
          while (is_msg_len && ((p + mlen_idx) < (p + len_end - 1))) {
              if (!(isdigit(*(p + mlen_idx)))) {is_msg_len = false;}
              mlen_idx++;
          }
          if (is_msg_len) {
              std::string mlenstr  = std::string(p + start, p + len_end);
              mlen = strtol(mlenstr.c_str(), NULL, 10);
          }
          LOG (DEBUG, "prepended message length: " << mlen);
          if (mlen != 0) {
            len_end += mlen;
          } else {
              while ((*(p + len_end - 1) != ']') && (len_end < len)){
                /* check for start of message if message length doesn't exist */
                if (*(p + end - 1 ) == '<'){
                  start = end - 1;
                }
                ++end;
                ++len_end;
              }
          }
      }
      end = len_end;

      // check if end of tcp buffer is reached and message delimiter ']' has arrived,
      // if not, wait till it comes in next tcp buffer
      if (((end > len) ||
         ((end == len) && (*(p + end - 1) != ']') )) && (sess_buf != NULL)) {
          /* limiting session buffer to 5KB */
          if (len-start < 5120) {
              sess_buf->append(std::string(p + start, p + len));
              LOG(DEBUG, "structured_syslog next sess_buf: "
                << *sess_buf << " len: "<< len-start);
              return true;
          }
          else {
              LOG(ERROR, "next session buffer length too high, discarding buffer!!"
                "[log msg buf truncated to 2048 bytes] next sess_buf: "
                << std::string(p + start, p + 2048) << " length: "<< len-start);
              return false;
          }
      }
      r = SyslogParser::parse_syslog (p + start, p + end, v);
      LOG(DEBUG, "structured_syslog: " << std::string(p + start, p + end) <<
                 " start: " << start << " end: " << end << " parsed " << r);
      if (r) {
          v.insert(std::pair<std::string, SyslogParser::Holder>("ip",
                SyslogParser::Holder("ip", ip)));
          std::stringstream lenss;
          lenss << SyslogParser::GetMapVal(v, "msglen", 0);
          std::string lenstr = lenss.str();
          int message_len = SyslogParser::GetMapVal(v, "msglen", len);
          if (message_len != (int)len) {
            start += lenstr.length() + 1;
          }
          LOG(DEBUG, "structured_syslog message_len: " << message_len);
          SyslogParser::PostParsing(v);
          if (StructuredSyslogPostParsing(v, config_obj, stat_db_callback, p + start, end-start, forwarder) == false)
          {
            LOG(DEBUG, "structured_syslog not handled");
          }
          if (message_len == (int)len) {
            start = end + 1;
          } else {
            start += message_len;
          }
      } else {
          LOG(ERROR, "structured_syslog parse failed for: " << std::string(p + start, p + end));
          start = end;
      }
      while (!v.empty()) {
          v.erase(v.begin());
      }
  } while (start<len);
  return r;
}

}  // namespace impl

class StructuredSyslogServer::StructuredSyslogServerImpl {
public:
    StructuredSyslogServerImpl(EventManager *evm, uint16_t port,
        const vector<string> &structured_syslog_tcp_forward_dst,
        const std::string &structured_syslog_kafka_broker,
        const std::string &structured_syslog_kafka_topic,
        const Options::Kafka &kafka_options,
        uint16_t structured_syslog_kafka_partitions,
        uint64_t structured_syslog_active_session_map_limit,
        ConfigClientCollector *config_client,
        StatWalker::StatTableInsertFn stat_db_callback) :
        udp_server_(new StructuredSyslogUdpServer(evm, port,
            stat_db_callback)),
        tcp_server_(new StructuredSyslogTcpServer(evm, port,
            stat_db_callback)),
        structured_syslog_config_(new StructuredSyslogConfig(config_client, structured_syslog_active_session_map_limit)) {
        if ((structured_syslog_tcp_forward_dst.size() != 0) || structured_syslog_kafka_broker != "") {
            forwarder_.reset(new StructuredSyslogForwarder (evm, structured_syslog_tcp_forward_dst,
                                                            structured_syslog_kafka_broker,
                                                            structured_syslog_kafka_topic,
                                                            kafka_options,
                                                            structured_syslog_kafka_partitions));
        } else {
            LOG(DEBUG, "forward destination not configured");
        }

    }

    ~StructuredSyslogServerImpl() {
    }

    StructuredSyslogConfig *GetStructuredSyslogConfig() {
        return structured_syslog_config_;
    }

    boost::shared_ptr<StructuredSyslogForwarder> GetStructuredSyslogForwarder() {
        return forwarder_;
    }
    bool Initialize() {
        if (udp_server_->Initialize(GetStructuredSyslogConfig(), GetStructuredSyslogForwarder())) {
            return tcp_server_->Initialize(GetStructuredSyslogConfig(), GetStructuredSyslogForwarder());
        } else {
            return false;
        }
    }

    void Shutdown() {
        udp_server_->Shutdown();
        UdpServerManager::DeleteServer(udp_server_);
        udp_server_ = NULL;
        tcp_server_->Shutdown();
        TcpServerManager::DeleteServer(tcp_server_);
        if (forwarder_ != NULL)
            forwarder_->Shutdown();
    }

    boost::asio::ip::udp::endpoint GetLocalEndpoint(
        boost::system::error_code *ec) {
        return udp_server_->GetLocalEndpoint(ec);
    }

private:
    //
    // StructuredSyslogUdpServer
    //
    class StructuredSyslogUdpServer : public UdpServer {
    public:
        StructuredSyslogUdpServer(EventManager *evm, uint16_t port,
            StatWalker::StatTableInsertFn stat_db_callback) :
            UdpServer(evm, kBufferSize),
            port_(port),
            stat_db_callback_(stat_db_callback) {
        }

        bool Initialize(StructuredSyslogConfig *config_obj,
                        boost::shared_ptr<StructuredSyslogForwarder> forwarder) {
            int count = 0;
            while (count++ < kMaxInitRetries) {
                if (UdpServer::Initialize(port_)) {
                    break;
                }
                sleep(1);
            }
            if (!(count < kMaxInitRetries)) {
                LOG(ERROR, "EXITING: StructuredSyslogUdpServer initialization failed "
                    << "for port " << port_);
                exit(1);
            }
            StartReceive();
            config_obj_ = config_obj;
            forwarder_ = forwarder;
            return true;
        }

        virtual void OnRead(const boost::asio::const_buffer &recv_buffer,
            const boost::asio::ip::udp::endpoint &remote_endpoint) {
            size_t recv_buffer_size(boost::asio::buffer_size(recv_buffer));
            if (!structured_syslog::impl::ProcessStructuredSyslog(boost::asio::buffer_cast<const uint8_t *>(recv_buffer),
                    recv_buffer_size, remote_endpoint.address(), stat_db_callback_, config_obj_, forwarder_,
                    boost::shared_ptr<std::string>())) {
                LOG(ERROR, "ProcessStructuredSyslog UDP FAILED for : " << remote_endpoint);
            } else {
                LOG(DEBUG, "ProcessStructuredSyslog UDP SUCCESS for : " << remote_endpoint);
            }

            DeallocateBuffer(recv_buffer);
        }

    private:

        static const int kMaxInitRetries = 5;
        static const int kBufferSize = 32 * 1024;

        uint16_t port_;
        StatWalker::StatTableInsertFn stat_db_callback_;
        StructuredSyslogConfig *config_obj_;
        boost::shared_ptr<StructuredSyslogForwarder> forwarder_;
    };

    class StructuredSyslogTcpServer;

    class StructuredSyslogTcpSession : public TcpSession {
    public:
        typedef boost::intrusive_ptr<StructuredSyslogTcpSession> StructuredSyslogTcpSessionPtr;
        StructuredSyslogTcpSession (StructuredSyslogTcpServer *server, Socket *socket) :
            TcpSession(server, socket) {
            sess_buf.reset(new std::string(""));
            //set_observer(boost::bind(&SyslogTcpSession::OnEvent, this, _1, _2));
        }
        virtual void OnRead (const boost::asio::const_buffer buf) {
            boost::system::error_code ec;
            StructuredSyslogTcpServer *sserver = dynamic_cast<StructuredSyslogTcpServer *>(server());
            //TODO: handle error
            sserver->ReadMsg(StructuredSyslogTcpSessionPtr(this), buf, socket ()->remote_endpoint(ec));
        }
        boost::shared_ptr<std::string> sess_buf;
    };

    //
    // StructuredSyslogTcpServer
    //
    class StructuredSyslogTcpServer : public TcpServer {
    public:
        typedef boost::intrusive_ptr<StructuredSyslogTcpSession> StructuredSyslogTcpSessionPtr;
        StructuredSyslogTcpServer(EventManager *evm, uint16_t port,
            StatWalker::StatTableInsertFn stat_db_callback) :
            TcpServer(evm),
            port_(port),
            session_(NULL),
            stat_db_callback_(stat_db_callback) {
        }

        virtual TcpSession *AllocSession(Socket *socket)
        {
            session_ = new StructuredSyslogTcpSession (this, socket);
            return session_;
        }

        bool Initialize(StructuredSyslogConfig *config_obj, boost::shared_ptr<StructuredSyslogForwarder> forwarder) {
            TcpServer::Initialize (port_);
            LOG(DEBUG, __func__ << " Initialization of TCP StructuredSyslog listener @" << port_);
            config_obj_ = config_obj;
            forwarder_ = forwarder;
            return true;
        }

        virtual void ReadMsg(StructuredSyslogTcpSessionPtr sess, const boost::asio::const_buffer &recv_buffer,
            const boost::asio::ip::tcp::endpoint &remote_endpoint) {
            size_t recv_buffer_size(boost::asio::buffer_size(recv_buffer));

            if (!structured_syslog::impl::ProcessStructuredSyslog(
                    boost::asio::buffer_cast<const uint8_t *>(recv_buffer),
                    recv_buffer_size, remote_endpoint.address(), stat_db_callback_, config_obj_,
                    forwarder_, sess->sess_buf)) {
                LOG(ERROR, "ProcessStructuredSyslog TCP FAILED for : " << remote_endpoint);
            } else {
                LOG(DEBUG, "ProcessStructuredSyslog TCP SUCCESS for : " << remote_endpoint);
            }

            //sess->server()->DeleteSession (sess.get());
            sess->ReleaseBuffer(recv_buffer);
        }

    private:
        uint16_t port_;
        StructuredSyslogTcpSession *session_;
        StatWalker::StatTableInsertFn stat_db_callback_;
        StructuredSyslogConfig *config_obj_;
        boost::shared_ptr<StructuredSyslogForwarder> forwarder_;
    };
    StructuredSyslogUdpServer *udp_server_;
    StructuredSyslogTcpServer *tcp_server_;
    boost::shared_ptr<StructuredSyslogForwarder> forwarder_;
    StructuredSyslogConfig *structured_syslog_config_;
};

StructuredSyslogServer::StructuredSyslogServer(EventManager *evm,
    uint16_t port, const vector<string> &structured_syslog_tcp_forward_dst,
    const std::string &structured_syslog_kafka_broker,
    const std::string &structured_syslog_kafka_topic,
    uint16_t structured_syslog_kafka_partitions,
    uint64_t structured_syslog_active_session_map_limit,
    const Options::Kafka &kafka_options,
    ConfigClientCollector *config_client,
    StatWalker::StatTableInsertFn stat_db_fn) {
    impl_ = new StructuredSyslogServerImpl(evm, port, structured_syslog_tcp_forward_dst,
                                           structured_syslog_kafka_broker,
                                           structured_syslog_kafka_topic,
                                           kafka_options,
                                           structured_syslog_kafka_partitions,
                                           structured_syslog_active_session_map_limit,
                                           config_client, stat_db_fn);
}

StructuredSyslogServer::~StructuredSyslogServer() {
    if (impl_) {
        delete impl_;
        impl_ = NULL;
    }
}

bool StructuredSyslogServer::Initialize() {
    return impl_->Initialize();
}

void StructuredSyslogServer::Shutdown() {
    impl_->Shutdown();
}

boost::asio::ip::udp::endpoint StructuredSyslogServer::GetLocalEndpoint(
    boost::system::error_code *ec) {
    return impl_->GetLocalEndpoint(ec);
}

//
// StructuredSyslogTcpForwarderSession
//
class StructuredSyslogTcpForwarderSession : public TcpSession {
private:
    StructuredSyslogTcpForwarder *server_;

public:
    StructuredSyslogTcpForwarderSession (StructuredSyslogTcpForwarder *server, Socket *socket) :
        TcpSession((TcpServer*)server, socket),
        server_(server){
    }

    virtual void OnRead (const boost::asio::const_buffer buf) {
        LOG(DEBUG, "StructuredSyslogTcpForwarderSession OnRead");
    }

    virtual void WriteReady(const boost::system::error_code &ec) {
        server_->WriteReady(ec);
    }
};

StructuredSyslogTcpForwarder::StructuredSyslogTcpForwarder(EventManager *evm, const std::string &ipaddress, int port) :
    TcpServer(evm),
    ipaddress_(ipaddress),
    port_(port),
    session_(NULL),
    ready_to_send_(true) {
}

TcpSession* StructuredSyslogTcpForwarder::AllocSession(Socket *socket) {
    session_ = new StructuredSyslogTcpForwarderSession(this, socket);
    return session_;
}

void StructuredSyslogTcpForwarder::WriteReady(const boost::system::error_code &ec) {
    LOG(DEBUG, "StructuredSyslogTcpForwarder::WriteReady");
    if (ec) {
        return;
    }
    {
        tbb::mutex::scoped_lock lock(send_mutex_);
        ready_to_send_ = true;
    }
}

void StructuredSyslogTcpForwarder::Connect() {
    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint endpoint;
    endpoint.address(AddressFromString(ipaddress_, &ec));
    endpoint.port(port_);
    TcpServer::Connect(session_, endpoint);
}

bool StructuredSyslogTcpForwarder::Send(const u_int8_t *data, size_t size, size_t *actual) {
    *actual = 0;
    tbb::mutex::scoped_lock lock(send_mutex_);
    if (ready_to_send_) {
        ready_to_send_ = session_->Send(data, size, actual);
    }
    return ready_to_send_;
}

bool StructuredSyslogTcpForwarder::Connected() {
    boost::system::error_code ec;
    Endpoint remote = session_->socket()->remote_endpoint(ec);
    return session_->Connected(remote);
}

void StructuredSyslogTcpForwarder::SetSocketOptions() {
    session_->SetSocketOptions();
}

StructuredSyslogForwarder::StructuredSyslogForwarder(EventManager *evm,
                                                     const vector <std::string> &tcp_forward_dst,
                                                     const std::string &structured_syslog_kafka_broker,
                                                     const std::string &structured_syslog_kafka_topic,
                                                     const Options::Kafka &kafka_options,
                                                     uint16_t structured_syslog_kafka_partitions):
    evm_(evm) {
    if (tcp_forward_dst.size() != 0) {
        tcpForwarder_poll_timer_= TimerManager::CreateTimer(*evm->io_service(),
                                  "tcpForwarder poll timer",
                                  TaskScheduler::GetInstance()->GetTaskId("tcpForwarder poller"));
        tcpForwarder_poll_timer_->Start(tcpForwarderPollInterval,
        boost::bind(&StructuredSyslogForwarder::PollTcpForwarder, this),
        boost::bind(&StructuredSyslogForwarder::PollTcpForwarderErrorHandler, this, _1, _2));
    }
    Init(tcp_forward_dst, structured_syslog_kafka_broker, structured_syslog_kafka_topic,
         kafka_options,
         structured_syslog_kafka_partitions);
}

void StructuredSyslogForwarder::Init(const std::vector<std::string> &tcp_forward_dst,
                                     const std::string &structured_syslog_kafka_broker,
                                     const std::string &structured_syslog_kafka_topic,
                                     const Options::Kafka &kafka_options,
                                     uint16_t structured_syslog_kafka_partitions) {
    for (std::vector<std::string>::const_iterator it = tcp_forward_dst.begin(); it != tcp_forward_dst.end(); ++it) {
        std::vector<std::string> dest;
        boost::split(dest, *it, boost::is_any_of(":"), boost::token_compress_on);
        StructuredSyslogTcpForwarder* fwder = new StructuredSyslogTcpForwarder(evm_,
                                                                               dest[0],
                                                                               atoi(dest[1].c_str()));
        fwder->CreateSession();
        fwder->Connect();
        fwder->SetSocketOptions();
        tcpForwarder_.push_back(fwder);
    }
    if (structured_syslog_kafka_broker != "") {
        kafkaForwarder_ = new KafkaForwarder(evm_, structured_syslog_kafka_broker,
                                             structured_syslog_kafka_topic,
                                             kafka_options,
                                             structured_syslog_kafka_partitions);
    } else {
        kafkaForwarder_ = NULL;
    }
}

StructuredSyslogForwarder::~StructuredSyslogForwarder() {
    Shutdown();
    if (tcpForwarder_poll_timer_) {
        TimerManager::DeleteTimer(tcpForwarder_poll_timer_);
        tcpForwarder_poll_timer_ = NULL;
    }
}

void StructuredSyslogForwarder::PollTcpForwarderErrorHandler(string error_name,
    string error_message) {
    LOG(ERROR, "PollTcpForwarder Timer Err: " << error_name << " " << error_message);
}

bool StructuredSyslogForwarder::PollTcpForwarder() {
    LOG(DEBUG, "PollTcpForwarder start");
    for (std::vector<StructuredSyslogTcpForwarder*>::iterator it = tcpForwarder_.begin();
         it != tcpForwarder_.end(); ++it) {
        if ((*it)->Connected() ==  false) {
            std::string dst = (*it)->GetIpAddress();
            int port = (*it)->GetPort();
            LOG(DEBUG,"reconnecting to remote syslog server " << dst << ":" << port);
            StructuredSyslogTcpForwarder* old_fwder = *it;
            StructuredSyslogTcpForwarder* new_fwder = new StructuredSyslogTcpForwarder(evm_, dst, port);
            new_fwder->CreateSession();
            new_fwder->Connect();
            new_fwder->SetSocketOptions();
            std::replace(tcpForwarder_.begin(),tcpForwarder_.end(), old_fwder, new_fwder);
            old_fwder->ClearSessions();
            TcpServerManager::DeleteServer(old_fwder);
        } else {
            LOG(DEBUG,"connection to remote syslog server " << (*it)->GetIpAddress() << ":"<< (*it)->GetPort() << " is fine");
        }
    }
    return true;
}

void StructuredSyslogForwarder::Shutdown() {
    if (kafkaForwarder_ != NULL) {
        kafkaForwarder_->Shutdown();
    }
    LOG(DEBUG, __func__ << " structured_syslog_forwarder shutdown done");
}

bool StructuredSyslogForwarder::kafkaForwarder() {
    return (kafkaForwarder_ != NULL);
}

void StructuredSyslogForwarder::Forward(boost::shared_ptr<StructuredSyslogQueueEntry> sqe) {
    for (std::vector<StructuredSyslogTcpForwarder*>::iterator it = tcpForwarder_.begin();
         it != tcpForwarder_.end(); ++it) {
        size_t bytes_written;
        (*it)->Send((const u_int8_t*)sqe->data->c_str(), sqe->length, &bytes_written);
        if (bytes_written < sqe->length) {
            LOG(DEBUG, "error writing - bytes_written: " << bytes_written);
        }
    }
    if (kafkaForwarder_ != NULL) {
        LOG(DEBUG, "forwarding json  - " << *(sqe->json_data));
        kafkaForwarder_->Send(*(sqe->json_data), *(sqe->skey));
    }
}

StructuredSyslogQueueEntry::StructuredSyslogQueueEntry(boost::shared_ptr<std::string> d, size_t len,
                                                       boost::shared_ptr<std::string> jd,
                                                       boost::shared_ptr<std::string> key):
        length(len), data(d), json_data(jd),skey(key) {
}

StructuredSyslogQueueEntry::~StructuredSyslogQueueEntry() {
}

}  // namespace structured_syslog
