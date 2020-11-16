/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/*
  Author(s):  Zihan Chen, Peter Kazanzides

  (C) Copyright 2014-2020 Johns Hopkins University (JHU), All Rights Reserved.

--- begin cisst license - do not edit ---

This software is provided "as is" under an open source license, with
no warranty.  The complete license can be found in license.txt and
http://www.cisst.org/cisst/license.txt.

--- end cisst license ---
*/


#ifndef __EthBasePort_H__
#define __EthBasePort_H__

#include <iostream>
#include "BasePort.h"

// Some useful constants related to the FireWire protocol
const unsigned int FW_QREAD_SIZE      = 16;        // Number of bytes in Firewire quadlet read request packet
const unsigned int FW_QWRITE_SIZE     = 20;        // Number of bytes in Firewire quadlet write packet
const unsigned int FW_QRESPONSE_SIZE  = 20;        // Number of bytes in Firewire quadlet read response packet
const unsigned int FW_BREAD_SIZE      = 20;        // Number of bytes in Firewire block read request
const unsigned int FW_BRESPONSE_HEADER_SIZE = 20;  // Number of bytes in Firewire block read response header (including header CRC)
const unsigned int FW_BWRITE_HEADER_SIZE = 20;     // Number of bytes in Firewire block write header (including header CRC)
const unsigned int FW_CRC_SIZE = 4;                // Number of bytes in Firewire CRC

// Firewire control word -- specific to our implementation.
// First byte is FW_CTRL_FLAGS, second byte is Firewire bus generation
const unsigned int FW_CTRL_SIZE = 2;               // Number of bytes in Firewire control word

// Firewire extra data, received after the quadlet and block responses. This is specific to our implementation.
const unsigned int FW_EXTRA_SIZE = 8;              // Number of extra bytes (4 words)

// FW_CTRL_FLAGS
const unsigned char FW_CTRL_NOFORWARD = 0x01;      // Prevent forwarding by Ethernet/Firewire bridge

class EthBasePort : public BasePort
{
public:
    //! FireWire tcode
    enum TCODE {
        QWRITE = 0,
        BWRITE = 1,
        QREAD = 4,
        BREAD = 5,
        QRESPONSE = 6,
        BRESPONSE = 7
    };

    typedef bool (*EthCallbackType)(EthBasePort &port, unsigned char boardId, std::ostream &debugStream);

protected:

    bool is_fw_master;      // Whether bridge board must manage Firewire bus (e.g., Firewire not connected to PC)
    uint8_t fw_tl;          // FireWire transaction label (6 bits)

    EthCallbackType eth_read_callback;
    double ReceiveTimeout;      // Ethernet receive timeout (seconds)

    bool   FwBusReset;          // True if Firewire bus reset is active

    double FPGA_RecvTime;       // Time for FPGA to receive Ethernet packet (seconds)
    double FPGA_TotalTime;      // Total time for FPGA to receive packet and respond (seconds)

    // Write a block to the specified node. Internal method called by ReadBlock.
    bool ReadBlockNode(nodeid_t node, nodeaddr_t addr, quadlet_t *rdata, unsigned int nbytes);

    // Write a block to the specified node. Internal method called by WriteBlock and
    // WriteAllBoardsBroadcast.
    bool WriteBlockNode(nodeid_t node, nodeaddr_t addr, quadlet_t *wdata, unsigned int nbytes);

    // Send packet
    virtual bool PacketSend(char *packet, size_t nbytes, bool useEthernetBroadcast) = 0;

    // Method called by ReadAllBoards/ReadAllBoardsBroadcast if no data read
    void OnNoneRead(void);

    // Method called by WriteAllBoards/WriteAllBoardsBroadcast if no data written
    void OnNoneWritten(void);

    // Method called when Firewire bus reset has caused the Firewire generation number on the FPGA
    // to be different than the one on the PC.
    virtual void OnFwBusReset(unsigned int FwBusGeneration_FPGA);

    void make_1394_header(quadlet_t *packet, nodeid_t node, nodeaddr_t addr, unsigned int tcode, unsigned int tl);

    void make_qread_packet(quadlet_t *packet, nodeid_t node, nodeaddr_t addr, unsigned int tl);
    void make_qwrite_packet(quadlet_t *packet, nodeid_t node, nodeaddr_t addr, quadlet_t data, unsigned int tl);
    void make_bread_packet(quadlet_t *packet, nodeid_t node, nodeaddr_t addr, unsigned int nBytes, unsigned int tl);
    void make_bwrite_packet(quadlet_t *packet, nodeid_t node, nodeaddr_t addr, quadlet_t *data, unsigned int nBytes, unsigned int tl);

public:

    EthBasePort(int portNum, std::ostream &debugStream = std::cerr, EthCallbackType cb = 0);

    ~EthBasePort();

    void SetEthCallback(EthCallbackType callback)
    { eth_read_callback = callback; }

    double GetReceiveTimeout(void) const { return ReceiveTimeout; }

    void SetReceiveTimeout(double timeSec) { ReceiveTimeout = timeSec; }

    // Return time required to receive Ethernet packet on FPGA, in seconds.
    double GetFpgaReceiveTime(void) const { return FPGA_RecvTime; }

    // Return time required to receive and respond to Ethernet packet on FPGA, in seconds.
    // This is the last data value placed in the response packet, but it is a little
    // lower (by about 0.7 microseconds) because it does not include the time to write this
    // value to the KSZ8851 or to queue the packet into the transmit buffer.
    double GetFpgaTotalTime(void) const { return FPGA_TotalTime; }

    //****************** Virtual methods ***************************
    // Implementations of pure virtual methods from BasePort

    // For now, always returns 1
    int NumberOfUsers(void) { return 1; }

    unsigned int GetBusGeneration(void) const { return FwBusGeneration; }

    void UpdateBusGeneration(unsigned int gen) { FwBusGeneration = gen; }

    // Read a quadlet from the specified board
    bool ReadQuadlet(unsigned char boardId, nodeaddr_t addr, quadlet_t &data);

    // Write a quadlet to the specified board
    bool WriteQuadlet(unsigned char boardId, nodeaddr_t addr, quadlet_t data);

    // Write a block to the specified board
    bool WriteBlock(unsigned char boardId, nodeaddr_t addr, quadlet_t *wdata, unsigned int nbytes);

    /*!
     \brief Write the broadcast read request
    */
    bool WriteBroadcastReadRequest(unsigned int seq);

    /*!
     \brief Wait for broadcast read data to be available
    */
    void WaitBroadcastRead(void);

    /*!
     \brief Add delay (if needed) for PROM I/O operations
     The delay is non-zero for Ethernet.
    */
    void PromDelay(void) const;

    //****************** Static methods ***************************

    // Returns the destination MAC address (6 bytes)
    // The first 3 characters are FA:61:OE, which is the CID assigned to LCSR by IEEE
    // The next 2 characters are 13:94
    // The last character is 0 (set it to the board address)
    static void GetDestMacAddr(unsigned char *macAddr);

    // Returns the destination multicast MAC address (6 bytes)
    // The first 3 characters are FB:61:OE, which is the CID assigned to LCSR by IEEE, with the multicast bit set
    // The last 3 characters are 13:94:FF
    static void GetDestMulticastMacAddr(unsigned char *macAddr);

    // Print MAC address
    static void PrintMAC(std::ostream &outStr, const char* name, const uint8_t *addr, bool swap16 = false);

    // Print IP address
    static void PrintIP(std::ostream &outStr, const char* name, const uint8_t *addr, bool swap16 = false);

    // Check if FireWire packet valid
    //   length:  length of data sectio (for BRESPONSE)
    //   node:    expected source node
    //   tcode:   expected tcode (e.g., QRESPONSE or BRESPONSE)
    //   tl:      transaction label
    // PK TODO: Make it static? Need to handle outStr
    bool CheckFirewirePacket(const unsigned char *packet, size_t length, nodeid_t node, unsigned int tcode, unsigned int tl);

    // Process extra data received from FPGA
    void ProcessExtraData(const unsigned char *packet);

    static bool checkCRC(const unsigned char *packet);

    // Print FireWire packet
    static void PrintFirewirePacket(std::ostream &out, const quadlet_t *packet, unsigned int max_quads);

    static void PrintEthernetPacket(std::ostream &out, const quadlet_t *packet, unsigned int max_quads);

    static void PrintDebug(std::ostream &debugStream, unsigned short status);

    // Print Ethernet debug data; clockPeriod is in seconds
    static void PrintDebugData(std::ostream &debugStream, const quadlet_t *data, double clockPeriod);
};

#endif  // __EthBasePort_H__
