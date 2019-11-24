/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/*
  Author(s):  Peter Kazanzides, Zihan Chen

  (C) Copyright 2014-2019 Johns Hopkins University (JHU), All Rights Reserved.

--- begin cisst license - do not edit ---

This software is provided "as is" under an open source license, with
no warranty.  The complete license can be found in license.txt and
http://www.cisst.org/cisst/license.txt.

--- end cisst license ---
*/

#include <stdio.h>
#include "BasePort.h"


BasePort::BasePort(int portNum, std::ostream &ostr):
        outStr(ostr),
        Protocol_(BasePort::PROTOCOL_SEQ_RW),
        IsAllBoardsBroadcastCapable_(false),
        IsAllBoardsBroadcastShorterWait_(false),
        IsNoBoardsBroadcastShorterWait_(true),
        ReadSequence_(0),
        PortNum(portNum),
        NumOfNodes_(0),
        NumOfBoards_(0),
        BoardInUseMask_(0),
        HubBoard(BoardIO::MAX_BOARDS)
{
    size_t i;
    for (i = 0; i < BoardIO::MAX_BOARDS; i++) {
        BoardExists[i] = false;
        BoardList[i] = 0;
        FirmwareVersion[i] = 0;
        Board2Node[i] = MAX_NODES;
    }
    for (i = 0; i < MAX_NODES; i++)
        Node2Board[i] = BoardIO::MAX_BOARDS;
}

void BasePort::SetProtocol(ProtocolType prot) {
    if (!IsAllBoardsBroadcastCapable_ && (prot != BasePort::PROTOCOL_SEQ_RW)) {
        outStr << "***Error: not all boards support broadcasting, " << std::endl
               << "          please upgrade your firmware"  << std::endl;
    } else {
        switch (prot) {
        case BasePort::PROTOCOL_SEQ_RW:
            outStr << "System running in NON broadcast mode" << std::endl;
            Protocol_ = prot;
            break;
        case BasePort::PROTOCOL_SEQ_R_BC_W:
            outStr << "System running with broadcast write" << std::endl;
            Protocol_ = prot;
            break;
        case BasePort::PROTOCOL_BC_QRW:
            outStr << "System running with broadcast query, read, and write" << std::endl;
            Protocol_ = prot;
            break;
        default:
            outStr << "Unknown protocol (ignored): " << prot << std::endl;
            break;
        }
    }
}

bool BasePort::AddBoard(BoardIO *board)
{
    int id = board->BoardId;
    if ((id < 0) || (id >= BoardIO::MAX_BOARDS)) {
        outStr << "BasePort::AddBoard: board number out of range: " << id << std::endl;
        return false;
    }
    BoardList[id] = board;
    board->port = this;

    // update BoardInUseMask_
    BoardInUseMask_ = (BoardInUseMask_ | (1 << id));

    NumOfBoards_++;   // increment board counts
    return true;
}

bool BasePort::RemoveBoard(unsigned char boardId)
{
    if (boardId >= BoardIO::MAX_BOARDS) {
        outStr << "BasePort::RemoveBoard: board number out of range: " << boardId << std::endl;
        return false;
    }
    BoardIO *board = BoardList[boardId];
    if (!board) {
        outStr << "BasePort::RemoveBoard: board not found: " << boardId << std::endl;
        return false;
    }

    // update BoardInuseMask_
    BoardInUseMask_ = (BoardInUseMask_ & (~(1 << boardId)));

    BoardList[boardId] = 0;
    board->port = 0;
    NumOfBoards_--;
    return true;
}

std::string BasePort::PortTypeString(PortType portType)
{
    if (portType == PORT_FIREWIRE)
        return std::string("Firewire");
    else if (portType == PORT_ETH_RAW)
        return std::string("Ethernet-Raw");
    else if (portType == PORT_ETH_UDP)
        return std::string("Ethernet-UDP");
    else
        return std::string("Unknown");
}

// Helper function for parsing command line options.
// In particular, this is typically called after a certain option, such as -p, is
// recognized and it parses the rest of that option string:
// N               for FireWire, where N is the port number (backward compatibility)
// fwN             for FireWire, where N is the port number
// ethN            for raw Ethernet (PCAP), where N is the port number
// udpxx.xx.xx.xx  for UDP, where xx.xx.xx.xx is the (optional) server IP address
bool BasePort::ParseOptions(const char *arg, PortType &portType, int &portNum, std::string &IPaddr)
{
    if (strncmp(arg, "fw", 2) == 0) {
        portType = PORT_FIREWIRE;
        return (sscanf(arg+2, "%d", &portNum) == 1);
    }
    else if (strncmp(arg, "eth", 3) == 0) {
        portType = BasePort::PORT_ETH_RAW;
        return (sscanf(arg+3, "%d", &portNum) == 1);
    }
    else if (strncmp(arg, "udp", 3) == 0) {
        portType = BasePort::PORT_ETH_UDP;
        // For now, if at least 8 characters, assume a valid IP address
        if (strlen(arg+3) >= 8)
            IPaddr.assign(arg+5);
        return true;
    }
    portType = PORT_FIREWIRE;
    return (sscanf(arg, "%d", &portNum) == 1);
}