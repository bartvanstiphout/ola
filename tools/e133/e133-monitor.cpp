/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * e133-monitor.cpp
 * Copyright (C) 2011 Simon Newton
 *
 * This locates all E1.33 devices using SLP and then opens a TCP connection to
 * each.  If --targets is used it skips the SLP step.
 *
 * It then waits to receive E1.33 messages on the TCP connections.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#include "plugins/e131/e131/E131Includes.h"  //  NOLINT, this has to be first
#include <errno.h>
#include <getopt.h>
#include <sysexits.h>

#include <ola/BaseTypes.h>
#include <ola/Callback.h>
#include <ola/Clock.h>
#include <ola/Logging.h>
#include <ola/StringUtils.h>
#include <ola/network/IPV4Address.h>
#include <ola/network/NetworkUtils.h>
#include <ola/network/SelectServer.h>
#include <ola/network/Socket.h>
#include <ola/rdm/PidStoreHelper.h>
#include <ola/rdm/RDMCommand.h>
#include <ola/rdm/RDMEnums.h>
#include <ola/rdm/RDMHelper.h>
#include <ola/rdm/UID.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include HASH_MAP_H

#include "plugins/e131/e131/ACNPort.h"
#include "plugins/e131/e131/CID.h"
#include "plugins/e131/e131/DMPE133Inflator.h"
#include "plugins/e131/e131/E133Inflator.h"
#include "plugins/e131/e131/E133Sender.h"
#include "plugins/e131/e131/RootInflator.h"
#include "plugins/e131/e131/RootSender.h"
#include "plugins/e131/e131/TCPTransport.h"

#include "tools/e133/E133HealthCheckedConnection.h"
#include "tools/e133/E133TCPConnector.h"
#include "tools/e133/SlpThread.h"
#include "tools/e133/SlpUrlParser.h"

using ola::NewCallback;
using ola::NewSingleCallback;
using ola::network::IPV4Address;
using ola::network::TcpSocket;
using ola::rdm::PidStoreHelper;
using ola::rdm::RDMRequest;
using ola::rdm::UID;
using ola::TimeInterval;
using ola::TimeStamp;
using std::auto_ptr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

typedef struct {
  bool help;
  ola::log_level log_level;
  string target_addresses;
  string pid_file;
} options;


/*
 * Parse our command line options
 */
void ParseOptions(int argc, char *argv[], options *opts) {
  static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},
      {"log-level", required_argument, 0, 'l'},
      {"pid-file", required_argument, 0, 'p'},
      {"targets", required_argument, 0, 't'},
      {0, 0, 0, 0}
    };

  int option_index = 0;

  while (1) {
    int c = getopt_long(argc, argv, "hl:p:t:", long_options, &option_index);

    if (c == -1)
      break;

    switch (c) {
      case 'h':
        opts->help = true;
        break;
      case 'l':
        switch (atoi(optarg)) {
          case 0:
            // nothing is written at this level
            // so this turns logging off
            opts->log_level = ola::OLA_LOG_NONE;
            break;
          case 1:
            opts->log_level = ola::OLA_LOG_FATAL;
            break;
          case 2:
            opts->log_level = ola::OLA_LOG_WARN;
            break;
          case 3:
            opts->log_level = ola::OLA_LOG_INFO;
            break;
          case 4:
            opts->log_level = ola::OLA_LOG_DEBUG;
            break;
          default :
            break;
        }
        break;
      case 'p':
        opts->pid_file = optarg;
        break;
      case 't':
        opts->target_addresses = optarg;
        break;
      case '?':
        break;
      default:
       break;
    }
  }
}


/*
 * Display the help message
 */
void DisplayHelpAndExit(char *argv[]) {
  cout << "Usage: " << argv[0] << " [options]\n"
  "\n"
  "Monitor E1.33 Devices.\n"
  "\n"
  "  -h, --help                Display this help message and exit.\n"
  "  -t, --targets <ip>,<ip>   List of IPs to connect to, overrides SLP\n"
  "  -p, --pid-file            The file to read PID definitiions from\n"
  "  -l, --log-level <level>   Set the logging level 0 .. 4.\n"
  << endl;
  exit(0);
}


/**
 * Tracks the TCP connection state to a remote node
 */
class NodeTCPState {
  public:
    NodeTCPState()
      : socket(NULL),
        health_checked_connection(NULL),
        connection_attempts(0) {
    }
    ~NodeTCPState() {
      delete socket;
    }

    // public for now
    TcpSocket *socket;
    E133HealthCheckedConnection *health_checked_connection;
    unsigned int connection_attempts;
};


/**
 * A very simple E1.33 Controller
 */
class SimpleE133Monitor {
  public:
    explicit SimpleE133Monitor(PidStoreHelper *pid_helper);
    ~SimpleE133Monitor();

    bool Init();
    void PopulateResponderList();
    void AddIP(const IPV4Address &ip_address);

    void Run();
    void Stop() { m_ss.Terminate(); }

  private:
    PidStoreHelper *m_pid_helper;
    ola::network::SelectServer m_ss;
    SlpThread m_slp_thread;
    E133TCPConnector m_connector;
    ola::network::LinearBackoffPolicy m_backoff_policy;

    // hash_map of ips to TCP Connection State
    typedef HASH_NAMESPACE::HASH_MAP_CLASS<uint32_t, NodeTCPState*> ip_map;
    ip_map m_ip_map;

    // The Controller's CID
    ola::plugin::e131::CID m_cid;

    // senders
    ola::plugin::e131::RootSender m_root_sender;
    ola::plugin::e131::E133Sender m_e133_sender;

    // inflators
    ola::plugin::e131::RootInflator m_root_inflator;
    ola::plugin::e131::E133Inflator m_e133_inflator;
    ola::plugin::e131::DMPE133Inflator m_dmp_inflator;

    // transports
    ola::plugin::e131::IncomingTCPTransport m_incoming_tcp_transport;

    /*
     * TODO: be careful about passing pointer to the NodeTCPState in callbacks
     * when we start removing stale entries this is going to break!
     * Maybe this won't be a problem since we'll never delete the entry for a
     * a node we have a connection to. Think about this.
     */
    void DiscoveryCallback(bool status, const vector<string> &urls);
    void OnTCPConnect(IPV4Address address, uint16_t port, TcpSocket *socket);
    void SocketUnhealthy(NodeTCPState *node_state);
    void SocketClosed(IPV4Address address);
    void E133DataReceived(const ola::plugin::e131::TransportHeader &header);

    void EndpointRequest(
        const ola::plugin::e131::TransportHeader &transport_header,
        const ola::plugin::e131::E133Header &e133_header,
        const string &raw_request);
    /*
    void RequestCallback(ola::rdm::rdm_response_code rdm_code,
                         const ola::rdm::RDMResponse *response,
                         const std::vector<std::string> &packets);
    */

    static const ola::TimeInterval TCP_CONNECT_TIMEOUT;
    static const ola::TimeInterval INITIAL_TCP_RETRY_DELAY;
    static const ola::TimeInterval MAX_TCP_RETRY_DELAY;
};

// 5 second connect() timeout
const ola::TimeInterval SimpleE133Monitor::TCP_CONNECT_TIMEOUT(5, 0);
const ola::TimeInterval SimpleE133Monitor::INITIAL_TCP_RETRY_DELAY(5, 0);
const ola::TimeInterval SimpleE133Monitor::MAX_TCP_RETRY_DELAY(60, 0);


SimpleE133Monitor::SimpleE133Monitor(
    PidStoreHelper *pid_helper)
    : m_pid_helper(pid_helper),
      m_slp_thread(
        &m_ss,
        ola::NewCallback(this, &SimpleE133Monitor::DiscoveryCallback)),
      m_connector(&m_ss,
                  NewCallback(this, &SimpleE133Monitor::OnTCPConnect),
                  TCP_CONNECT_TIMEOUT),
      m_backoff_policy(INITIAL_TCP_RETRY_DELAY, MAX_TCP_RETRY_DELAY),
      m_cid(ola::plugin::e131::CID::Generate()),
      m_root_sender(m_cid),
      m_e133_sender(&m_root_sender),
      m_dmp_inflator(NewCallback(this, &SimpleE133Monitor::E133DataReceived)),
      m_incoming_tcp_transport(&m_root_inflator) {
  m_root_inflator.AddInflator(&m_e133_inflator);
  m_e133_inflator.AddInflator(&m_dmp_inflator);

  m_dmp_inflator.SetRDMHandler(
      0,
      NewCallback(this, &SimpleE133Monitor::EndpointRequest));
}


SimpleE133Monitor::~SimpleE133Monitor() {
  // close out all tcp sockets and free state
  ip_map::iterator iter = m_ip_map.begin();
  for ( ; iter != m_ip_map.end(); ++iter) {
    delete iter->second;
  }
  m_ip_map.clear();

  m_slp_thread.Join();
  m_slp_thread.Cleanup();
}


bool SimpleE133Monitor::Init() {
  if (!m_slp_thread.Init()) {
    OLA_WARN << "SlpThread Init() failed";
    return false;
  }

  m_slp_thread.Start();
  return true;
}


/**
 * Locate the responder
 */
void SimpleE133Monitor::PopulateResponderList() {
  m_slp_thread.Discover();
}


void SimpleE133Monitor::AddIP(const IPV4Address &ip_address) {
  ip_map::iterator iter = m_ip_map.find(ip_address.AsInt());
  if (iter != m_ip_map.end()) {
    // the IP already exists
    return;
  }

  OLA_INFO << "Opening TCP connection to " << ip_address << ":" <<
    ola::plugin::e131::E133_PORT;

  NodeTCPState *node_state = new NodeTCPState();
  node_state->connection_attempts++;
  m_ip_map[ip_address.AsInt()] = node_state;

  // start the non-blocking connect
  m_connector.AddEndpoint(
      ip_address,
      ola::plugin::e131::E133_PORT,
      &m_backoff_policy);
}


/**
 * Run the controller and wait for the responses (or timeouts)
 */
void SimpleE133Monitor::Run() {
  m_ss.Run();
}


/**
 * Called when SLP completes discovery.
 */
void SimpleE133Monitor::DiscoveryCallback(bool ok,
                                          const vector<string> &urls) {
  if (ok) {
    vector<string>::const_iterator iter;
    UID uid(0, 0);
    IPV4Address ip;
    for (iter = urls.begin(); iter != urls.end(); ++iter) {
      OLA_INFO << "Located " << *iter;
      if (!ParseSlpUrl(*iter, &uid, &ip))
        continue;

      if (uid.IsBroadcast()) {
        OLA_WARN << "UID " << uid << "@" << ip << " is broadcast";
        continue;
      }
      AddIP(ip);
    }
  } else {
    OLA_INFO << "SLP discovery failed";
  }
}


/**
 * Called when a TCP socket is connected.
 */
void SimpleE133Monitor::OnTCPConnect(IPV4Address ip_address,
                                     uint16_t,
                                     TcpSocket *socket) {
  ip_map::iterator iter = m_ip_map.find(ip_address.AsInt());
  if (iter == m_ip_map.end()) {
    OLA_FATAL << "Unable to locate socket for " << ip_address;
    if (socket) {
      socket->Close();
      delete socket;
    }
    return;
  }

  iter->second->socket = socket;

  // setup the health checked channel here
  TimeInterval heartbeat_interval(2, 0);
  E133HealthCheckedConnection *health_checked_connection =
      new E133HealthCheckedConnection(
          &m_e133_sender,
          NewSingleCallback(this,
                            &SimpleE133Monitor::SocketUnhealthy,
                            iter->second),
          socket,
          &m_ss,
          heartbeat_interval);
  if (!health_checked_connection->Setup()) {
    OLA_WARN << "Failed to setup heartbeat controller for " << ip_address;
    delete health_checked_connection;
    socket->Close();
    delete socket;
    iter->second->socket = NULL;
    return;
  }

  if (iter->second->health_checked_connection) {
    // warn
    OLA_WARN << "pre-existing health_checked_connection for " << ip_address <<
        ", this is a bug and we'll leak memory!";
  }
  iter->second->health_checked_connection = health_checked_connection;
  socket->SetOnClose(
    NewSingleCallback(this, &SimpleE133Monitor::SocketClosed, ip_address));

  socket->SetOnData(
      NewCallback(&m_incoming_tcp_transport,
                  &ola::plugin::e131::IncomingTCPTransport::Receive,
                  socket));
  m_ss.AddReadDescriptor(socket);
}


/**
 * Called when a connection is deemed unhealthy.
 */
void SimpleE133Monitor::SocketUnhealthy(NodeTCPState *node_state) {
  OLA_INFO << "connection went unhealthy";
  delete node_state->health_checked_connection;
  node_state->health_checked_connection = NULL;

  // TODO(simon): clean this up
  IPV4Address peer_address;
  uint16_t port;
  node_state->socket->GetPeer(&peer_address, &port);
  SocketClosed(peer_address);
}


/**
 * Called when a socket is closed
 */
void SimpleE133Monitor::SocketClosed(IPV4Address ip_address) {
  OLA_INFO << "connection to " << ip_address << " was closed";

  ip_map::iterator iter = m_ip_map.find(ip_address.AsInt());
  if (iter == m_ip_map.end()) {
    OLA_FATAL << "Unable to locate socket for " << ip_address;
    return;
  }

  delete iter->second->health_checked_connection;
  iter->second->health_checked_connection = NULL;

  TcpSocket *socket = iter->second->socket;
  m_ss.RemoveReadDescriptor(socket);
  delete socket;
  iter->second->socket = NULL;
  m_ss.Terminate();
}


/**
 * Called when we receive E1.33 data. If this arrived over TCP we notify the
 * health checked connection.
 */
void SimpleE133Monitor::E133DataReceived(
    const ola::plugin::e131::TransportHeader &header) {
  if (header.Transport() != ola::plugin::e131::TransportHeader::TCP)
    return;

  ip_map::iterator iter = m_ip_map.find(header.SourceIP().AsInt());
  if (iter == m_ip_map.end()) {
    OLA_FATAL << "Received data but unable to lookup socket for " <<
      header.SourceIP();
    return;
  }

  if (iter->second->health_checked_connection)
    iter->second->health_checked_connection->HeartbeatReceived();
}


/**
 * We received data to endpoint 0
 */
void SimpleE133Monitor::EndpointRequest(
    const ola::plugin::e131::TransportHeader &transport_header,
    const ola::plugin::e131::E133Header &e133_header,
    const string &raw_request) {

  OLA_INFO << "got message from " << transport_header.SourceIP();
  (void) e133_header;
  (void) raw_request;
}


/**
 * Called when the RDM command completes
void SimpleE133Monitor::RequestCallback(
    ola::rdm::rdm_response_code rdm_code,
    const ola::rdm::RDMResponse *response,
    const std::vector<std::string> &packets) {
  cout << "RDM callback executed with code: " <<
    ola::rdm::ResponseCodeToString(rdm_code) << endl;

  if (!--m_responses_to_go)
    m_ss.Terminate();

  if (rdm_code == ola::rdm::RDM_COMPLETED_OK) {
    const ola::rdm::PidDescriptor *pid_descriptor = m_pid_helper->GetDescriptor(
        response->ParamId(),
        response->SourceUID().ManufacturerId());
    const ola::messaging::Descriptor *descriptor = NULL;
    const ola::messaging::Message *message = NULL;

    if (pid_descriptor) {
      switch (response->CommandClass()) {
        case ola::rdm::RDMCommand::GET_COMMAND_RESPONSE:
          descriptor = pid_descriptor->GetResponse();
          break;
        case ola::rdm::RDMCommand::SET_COMMAND_RESPONSE:
          descriptor = pid_descriptor->SetResponse();
          break;
        default:
          OLA_WARN << "Unknown command class " << response->CommandClass();
      }
    }
    if (descriptor) {
      message = m_pid_helper->DeserializeMessage(descriptor,
                                                 response->ParamData(),
                                                 response->ParamDataSize());
    }


    if (message) {
      cout << response->SourceUID() << " -> " << response->DestinationUID() <<
        endl;
      cout << m_pid_helper->MessageToString(message);
    } else {
      cout << response->SourceUID() << " -> " << response->DestinationUID()
        << ", TN: " << static_cast<int>(response->TransactionNumber()) <<
        ", Msg Count: " << static_cast<int>(response->MessageCount()) <<
        ", sub dev: " << response->SubDevice() << ", param 0x" << std::hex <<
        response->ParamId() << ", data len: " <<
        std::dec << static_cast<int>(response->ParamDataSize()) << endl;
    }

    if (message)
      delete message;
  }
  delete response;

  (void) packets;
}
 */


/*
 * Startup a node
 */
int main(int argc, char *argv[]) {
  options opts;
  opts.pid_file = PID_DATA_FILE;
  opts.log_level = ola::OLA_LOG_WARN;
  opts.help = false;
  ParseOptions(argc, argv, &opts);
  PidStoreHelper pid_helper(opts.pid_file);

  if (opts.help)
    DisplayHelpAndExit(argv);

  ola::InitLogging(opts.log_level, ola::OLA_LOG_STDERR);

  vector<IPV4Address> targets;
  if (!opts.target_addresses.empty()) {
    vector<string> tokens;
    ola::StringSplit(opts.target_addresses, tokens, ",");

    vector<string>::const_iterator iter = tokens.begin();
    for (; iter != tokens.end(); ++iter) {
      IPV4Address ip_address;
      if (!IPV4Address::FromString(*iter, &ip_address)) {
        OLA_WARN << "Invalid address " << *iter;
        DisplayHelpAndExit(argv);
      }
      targets.push_back(ip_address);
    }
  }

  if (!pid_helper.Init())
    exit(EX_OSFILE);

  SimpleE133Monitor monitor(&pid_helper);
  if (!monitor.Init())
    exit(EX_UNAVAILABLE);

  if (targets.empty()) {
    // this blocks while the slp thread does it's thing
    monitor.PopulateResponderList();
  } else {
    // manually add the responder IPs
    vector<IPV4Address>::const_iterator iter = targets.begin();
    for (; iter != targets.end(); ++iter)
      monitor.AddIP(*iter);
  }

  monitor.Run();
}