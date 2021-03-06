/** -*- c++ -*-
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 * 
 * This file is part of Hypertable.
 * 
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 * 
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <iostream>

extern "C" {
#include <poll.h>
}

#include "Common/Error.h"
#include "Common/Exception.h"
#include "Common/StringExt.h"

#include "AsyncComm/ApplicationQueue.h"
#include "AsyncComm/ResponseCallback.h"

#include "Hypertable/Lib/MasterClient.h"
#include "Hypertable/Lib/RangeServerProtocol.h"

#include "RequestHandlerCompact.h"
#include "RequestHandlerDestroyScanner.h"
#include "RequestHandlerDumpStats.h"
#include "RequestHandlerLoadRange.h"
#include "RequestHandlerUpdate.h"
#include "RequestHandlerCreateScanner.h"
#include "RequestHandlerFetchScanblock.h"
#include "RequestHandlerDropTable.h"
#include "RequestHandlerStatus.h"
#include "RequestHandlerReplayStart.h"
#include "RequestHandlerReplayUpdate.h"
#include "RequestHandlerReplayCommit.h"
#include "RequestHandlerDropRange.h"

#include "ConnectionHandler.h"
#include "EventHandlerMasterConnection.h"
#include "RangeServer.h"


using namespace Hypertable;

/**
 *
 */
ConnectionHandler::ConnectionHandler(Comm *comm, ApplicationQueuePtr &appQueuePtr, RangeServerPtr rangeServerPtr, MasterClientPtr &masterClientPtr) : m_comm(comm), m_app_queue_ptr(appQueuePtr), m_range_server_ptr(rangeServerPtr), m_master_client_ptr(masterClientPtr), m_shutdown(false) {
  return;
}

/**
 *
 */
ConnectionHandler::ConnectionHandler(Comm *comm, ApplicationQueuePtr &appQueuePtr, RangeServerPtr rangeServerPtr) : m_comm(comm), m_app_queue_ptr(appQueuePtr), m_range_server_ptr(rangeServerPtr), m_shutdown(false) {
  return;
}


/**
 *
 */
void ConnectionHandler::handle(EventPtr &event_ptr) {
  short command = -1;

  if (m_shutdown)
    return;

  if (event_ptr->type == Event::MESSAGE) {
    ApplicationHandler *requestHandler = 0;

    //event_ptr->display();

    try {
      if (event_ptr->messageLen < sizeof(int16_t)) {
	std::string message = "Truncated Request";
	throw new ProtocolException(message);
      }
      memcpy(&command, event_ptr->message, sizeof(int16_t));

      // sanity check command code
      if (command < 0 || command >= RangeServerProtocol::COMMAND_MAX) {
	std::string message = (std::string)"Invalid command (" + command + ")";
	throw ProtocolException(message);
      }

      switch (command) {
      case RangeServerProtocol::COMMAND_COMPACT:
	requestHandler = new RequestHandlerCompact(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_LOAD_RANGE:
	requestHandler = new RequestHandlerLoadRange(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_UPDATE:
	requestHandler = new RequestHandlerUpdate(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_CREATE_SCANNER:
	requestHandler = new RequestHandlerCreateScanner(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_DESTROY_SCANNER:
	requestHandler = new RequestHandlerDestroyScanner(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_FETCH_SCANBLOCK:
	requestHandler = new RequestHandlerFetchScanblock(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_DROP_TABLE:
	requestHandler = new RequestHandlerDropTable(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_REPLAY_START:
	requestHandler = new RequestHandlerReplayStart(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_REPLAY_UPDATE:
	requestHandler = new RequestHandlerReplayUpdate(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_REPLAY_COMMIT:
	requestHandler = new RequestHandlerReplayCommit(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_DROP_RANGE:
	requestHandler = new RequestHandlerDropRange(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_STATUS:
	requestHandler = new RequestHandlerStatus(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      case RangeServerProtocol::COMMAND_SHUTDOWN:
	m_shutdown = true;
	HT_INFO("Executing SHUTDOWN command.");
	{
	  ResponseCallback cb(m_comm, event_ptr);
	  cb.response_ok();
	}
	m_app_queue_ptr->shutdown();
	m_app_queue_ptr->join();
	m_app_queue_ptr = 0;
	m_range_server_ptr = 0;
	m_master_client_ptr = 0;
	return;
      case RangeServerProtocol::COMMAND_DUMP_STATS:
	requestHandler = new RequestHandlerDumpStats(m_comm, m_range_server_ptr.get(), event_ptr);
	break;
      default:
	std::string message = (string)"Command code " + command + " not implemented";
	throw ProtocolException(message);
      }
      m_app_queue_ptr->add( requestHandler );
    }
    catch (ProtocolException &e) {
      ResponseCallback cb(m_comm, event_ptr);
      std::string errMsg = e.what();
      HT_ERRORF("Protocol error '%s'", e.what());
      cb.error(Error::PROTOCOL_ERROR, errMsg);
    }
  }
  else if (event_ptr->type == Event::CONNECTION_ESTABLISHED) {

    HT_INFOF("%s", event_ptr->toString().c_str());

    /**
     * If this is the connection to the Master, then we need to register ourselves
     * with the master
     */
    if (m_master_client_ptr)
      m_app_queue_ptr->add( new EventHandlerMasterConnection(m_master_client_ptr, m_range_server_ptr->get_location(), event_ptr) );
  }
  else {
    HT_INFOF("%s", event_ptr->toString().c_str());
  }

}

