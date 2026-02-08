#include "RUDPListener.h"
#include <algorithm>	// For sort
#include <random>
#include <string>

#include "agk.h"		// For AGK functions

// Debug output macro - disable in release builds for performance
#ifdef _DEBUG
	#define RUDP_DEBUG(msg) OutputDebugStringA(msg)
#else
	#define RUDP_DEBUG(msg)
#endif


RUDPListener::RUDPListener(const std::string address, int port)
{
	// Initialize the listener with the given address and port
	this->address = address;
	this->port = port; 

	this->listenerUUID = GetUUID();

	AGKListener = agk::CreateUDPListener(address.c_str(), port);
}

RUDPListener::~RUDPListener()
{
	//Delete each connection pointer in the connection map
	for (const auto& entry : connectionMap)
	{
		//Memblocks in packets need to be deleted manually!
		for (Packet* packet : entry.second->readyPackets)
		{
			agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
			delete packet;
		}

		for (Packet* packet : entry.second->pendingPackets)
		{
			agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
			delete packet;
		}

		for (auto& pair : entry.second->outboundPackets)
		{
			agk::DeleteMemblock(pair.second->data); // Delete the memblock associated with the packet
			delete pair.second;
		}

		for (Packet* packet : entry.second->queuedOutbound)
		{
			agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
			delete packet;
		}

		// Clean up fragmentMap
		for (auto& fragmentEntry : entry.second->fragmentMap)
		{
			for (Packet* packet : fragmentEntry.second)
			{
				agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
				delete packet;
			}
		}

		//Connection struct destroy destroys remaining references
		delete entry.second;	
	}

	agk::DeleteUDPListener(AGKListener);
}

//Needs to be called every frame
void RUDPListener::Update()
{
	ProcessOutboundQueue(); // Process queued packets with bandwidth limiting (must be first)
	ReadIncomingMessages();
	UpdatePendingMessages();
	CheckOutgoingMessages();
	CheckTimeout();
	SendHeartbeats();
}

const std::string RUDPListener::GetListenerIP() const
{
	return address;
}

int RUDPListener::GetListenerPort() const
{
	return port;
}

RUDPListener::ConnectionUUID RUDPListener::GetListenerUUID() const
{
	return listenerUUID;
}

/*
 * Settings
 */

void RUDPListener::SetMessageTimeout(std::chrono::milliseconds timeout)
{
	OUTBOUND_TIMEOUT = timeout;
}

void RUDPListener::SetConnectionTimeout(std::chrono::milliseconds timeout)
{
	CONNECTION_TIMEOUT = timeout;
}

void RUDPListener::SetHeartbeatInterval(std::chrono::milliseconds interval)
{
	HEARTBEAT_INTERVAL = interval;
}

void RUDPListener::SetMaxThroughputPerFrame(int bytes)
{
	MAX_SEND_PER_FRAME = bytes;
}

/*
 * Active Connections
 */
RUDPListener::ConnectionUUID RUDPListener::GetConnectionUUID(int position) const
{
	for (const auto& entry : connectionMap)
	{
		if (position == 0)
		{
			return entry.first; // Return the UUID of the connection at the given position
		}
		position--;
	}
	return ""; // Return empty string if position is invalid
}

RUDPListener::ConnectionUUID RUDPListener::GetConnectionByAddress(const std::string ip, const int port) const
{
	for (const auto& entry : connectionMap)
	{
		Connection* connection = entry.second;
		if (connection->ip == ip && connection->port == port)
		{
			return entry.first; // Return the UUID of the connection with the given IP and port
		}
	}
	return ""; // Return empty string if no matching connection is found
}

unsigned int RUDPListener::GetAGKID() const
{
	return AGKListener; // Return the ID of the AGK listener
}

size_t RUDPListener::GetTotalConnections() const
{
	return connectionMap.size();
}

const std::string RUDPListener::GetConnectionIP(ConnectionUUID uuid) const
{
	auto entry = connectionMap.find(uuid);

	if (entry != connectionMap.end())
	{
		return entry->second->ip;
	}
	return ""; // Return empty string if the connection does not exist
}

int RUDPListener::GetConnectionPort(ConnectionUUID uuid) const
{
	auto entry = connectionMap.find(uuid);

	if (entry != connectionMap.end())
	{
		return entry->second->port;
	}
	return -1; // Return -1 if the connection does not exist
}

int RUDPListener::GetMaxThroughputPerFrame() const
{
	return MAX_SEND_PER_FRAME;
}

/*
	Public Methods
*/

void RUDPListener::Connect(const std::string ip, int port) const
{
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_CONNECT);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::SendUDPNetworkMessage(AGKListener, message, ip.c_str(), port);
}

void RUDPListener::Disconnect(ConnectionUUID uuid)
{
	//Try to send a disconnect message, we don't check if it fails since we'll timeout anyway
	auto entry = connectionMap.find(uuid);

	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		// Send an acknowledgment back to the sender
		unsigned int message = agk::CreateNetworkMessage();
		agk::AddNetworkMessageByte(message, MessageType::MSG_DISCONNECT);
		agk::AddNetworkMessageString(message, listenerUUID.c_str());
		agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);

		RemoveConnection(uuid); // Remove the connection from the map	
	}
}

int RUDPListener::GetMessage(ConnectionUUID uuid)
{
	auto entry = connectionMap.find(uuid);

	//If we find the uuid
	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		//If the packet list has messages ready
		if (!connection->readyPackets.empty())
		{
			Packet* packet = connection->readyPackets.front();
			connection->readyPackets.erase(connection->readyPackets.begin());
			unsigned int data = packet->data;
			delete packet; // Clean up the packet after processing
			return data; // Return the sequence or any other relevant data
		}
	}
	return -1;	//Return -1 if no message is ready
}

void RUDPListener::SendMessage(ConnectionUUID uuid, unsigned int memblock)
{
	auto entry = connectionMap.find(uuid);

	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		int data_size = agk::GetMemblockSize(memblock);
		const int FRAGMENT_SIZE = 1250;

		// Check if we need to fragment
		if (data_size <= FRAGMENT_SIZE)
		{
			// Small message - queue for sending
			Packet* packet = EncodePacket(listenerUUID, memblock);
			packet->sequence = connection->outboundSequence++;
			connection->queuedOutbound.push_back(packet); // Queue instead of immediate send
		}
		else
		{
			// Large message - fragment and queue all fragments
			int total_fragments = static_cast<int>(std::ceil(static_cast<float>(data_size) / FRAGMENT_SIZE));
			int base_sequence = connection->outboundSequence;

			for (int i = 0; i < total_fragments; i++)
			{
				int offset = i * FRAGMENT_SIZE;
				int chunk_size = std::min(FRAGMENT_SIZE, data_size - offset);

				// Create fragment packet
				Packet* fragment = EncodeFragmentPacket(
					listenerUUID, 
					memblock, 
					offset, 
					chunk_size, 
					base_sequence, 
					i, 
					total_fragments, 
					data_size
				);

				fragment->sequence = connection->outboundSequence++;
				connection->queuedOutbound.push_back(fragment); // Queue for bandwidth-limited sending
			}
		}
	}
}

size_t RUDPListener::GetMessageCount(ConnectionUUID uuid)
{
	auto entry = connectionMap.find(uuid);

	//If we find the uuid
	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		return connection->readyPackets.size();
	}
	return -1;	//Return -1 if no message is ready
}
/*
*	Updates
*/

void RUDPListener::ReadIncomingMessages()
{
	int udp_message = agk::GetUDPNetworkMessage(AGKListener);

	// Process all UDP messages received
	while (udp_message != 0)
	{	
		MessageType message_type = (MessageType)agk::GetNetworkMessageByte(udp_message); 

		switch (message_type)
		{
			case MSG_DATA:
			{
				ReadDataMessage(udp_message);
				break;
			}

			case MSG_ACK:
			{
				ReadACKMessage(udp_message);
				break;
			}

			case MSG_CONNECT:
			{
				ReadConnectMessage(udp_message);
				break;
			}

			case MSG_DISCONNECT:
			{
				ReadDisconnectMessage(udp_message);
				break;
			}

			case MSG_HANDSHAKE:
			{
				ReadHandshakeMessage(udp_message);
				break;
			}

			case MSG_HEARTBEAT:
			{
				ReadHeartbeatMessage(udp_message);
				break;
			}
		}

		agk::DeleteNetworkMessage(udp_message);	//Needs to be deleted
		udp_message = agk::GetUDPNetworkMessage(AGKListener);
	}
}

void RUDPListener::UpdatePendingMessages()
{
	// Iterate through all connections and check for pending packets
	for (auto& entry : connectionMap)
	{
		Connection* connection = entry.second;

		// First, try to reassemble any complete fragment groups
		ReassembleFragments(connection);

		// Check if there are any pending packets
		if (!connection->pendingPackets.empty())
		{
			//Sort pending packets by sequence number
			std::sort(connection->pendingPackets.begin(), connection->pendingPackets.end(),
				[](const Packet* a, const Packet* b) {
					return a->sequence < b->sequence;
				});

			for (auto it = connection->pendingPackets.begin(); it != connection->pendingPackets.end();)
			{
				Packet* packet = *it;
				// If the sequence is the next expected one, move it to ready packets
				if (packet->sequence == connection->inboundSequence)
				{
					// Check if this is a reassembled fragment packet
					if (packet->total_fragments > 0)
					{
						RUDP_DEBUG(("Moving reassembled packet to ready: sequence=" + std::to_string(packet->sequence) + 
						", incrementing inboundSequence by " + std::to_string(packet->total_fragments) + "\n").c_str());
						// Reassembled packet - increment by number of fragments consumed
						connection->inboundSequence += packet->total_fragments;
					}
					else
					{
						// Normal packet - increment by 1
						connection->inboundSequence++;
					}

					connection->readyPackets.push_back(packet);
					it = connection->pendingPackets.erase(it); // Remove from pending
				}
				else
				{
					RUDP_DEBUG(("Packet stuck in pending: packet->sequence=" + std::to_string(packet->sequence) + 
					", expected inboundSequence=" + std::to_string(connection->inboundSequence) + "\n").c_str());
					it++; // Move to the next packet
				}
			}
		}
	}
}

void RUDPListener::ProcessOutboundQueue()
{
	// Process each connection's outbound queue
	for (auto& entry : connectionMap)
	{
		Connection* connection = entry.second;

		// Reset send budget at start of frame
		connection->sendBudgetThisFrame = MAX_SEND_PER_FRAME;

		// Process queued packets up to bandwidth limit
		while (!connection->queuedOutbound.empty() && connection->sendBudgetThisFrame > 0)
		{
			Packet* packet = connection->queuedOutbound.front();

			// Calculate packet overhead (headers + payload)
			// UDP header ~8 bytes, IP header ~20 bytes, MSG_DATA metadata ~100 bytes
			int packetSize = packet->size + 128; 

			// Check if we have budget for this packet
			if (packetSize <= connection->sendBudgetThisFrame || connection->queuedOutbound.size() == 1)
			{
				// Send the packet
				SendDataMessage(connection, packet);
				connection->outboundPackets[packet->sequence] = packet; // Track for ACK
				connection->sendBudgetThisFrame -= packetSize;

				// Remove from queue
				connection->queuedOutbound.erase(connection->queuedOutbound.begin());
			}
			else
			{
				// Budget exhausted, stop processing this connection
				break;
			}
		}
	}
}

void RUDPListener::CheckOutgoingMessages()
{
	// Iterate through all connections and check for outbound packets
	for (auto& entry : connectionMap)
	{
		Connection* connection = entry.second;

		// Iterate through outbound packet map
		for (auto& packetPair : connection->outboundPackets)
		{
			Packet* packet = packetPair.second;
			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

			//Trigger resend
			if (std::chrono::duration_cast<std::chrono::milliseconds> (now - packet->timestamp) >= OUTBOUND_TIMEOUT)
			{
				SendDataMessage(connection, packet);
			}
		}

	}
}

void RUDPListener::CheckTimeout()
{
	// Iterate through all connections and check for outbound packets
	auto entry = connectionMap.begin();

	while (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - connection->lastUpdate) >= CONNECTION_TIMEOUT)
		{
			entry = RemoveConnection(entry->first);
		}
		else
			entry++;
	}
}

void RUDPListener::SendHeartbeats()
{
	// Iterate through all connections and send heartbeat messages
	for (auto& entry : connectionMap)
	{
		Connection* connection = entry.second;

		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - connection->lastHeartbeatSent) >= HEARTBEAT_INTERVAL)
		{
			SendHeartbeat(connection, 0); // Sequence not used for heartbeats
			connection->lastHeartbeatSent = now; //Update lastHeartbeatSent after sending
		}
	}
}


/*
* Read Messages by header type
*/

void RUDPListener::ReadDataMessage(int message)
{
	Packet* packet = DecodeMessage(message);

	//Continue placing package if its valid
	if (packet != nullptr)
	{
		auto entry = connectionMap.find(packet->uuid);

		//If we found our connection in the map
		if (entry != connectionMap.end())
		{
			Connection* connection = entry->second;

			connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time

			// Handle fragments separately
			if (packet->is_fragment)
			{
				// Add fragment to the fragment map
				connection->fragmentMap[packet->base_sequence].push_back(packet);
				SendAcknowledgment(connection, packet->sequence);
				// Don't increment inboundSequence here - fragments are out of normal flow

				RUDP_DEBUG(("Received packet fragment " + std::to_string(packet->fragment_index) + "/" + std::to_string(packet->total_fragments)+"\n").c_str());
			}
			else
			{
				// Normal packet handling
				//Check if the sequence is valid
				if (packet->sequence == connection->inboundSequence)
				{
					connection->inboundSequence++; //Increment sequence for next message
					connection->readyPackets.push_back(packet); //Add packet to ready packets
					SendAcknowledgment(connection, packet->sequence);
				}
				else if (packet->sequence > connection->inboundSequence)
				{
					connection->pendingPackets.push_back(packet); //Add packet to pending packets
					SendAcknowledgment(connection, packet->sequence);
				}
				else
				{
					SendAcknowledgment(connection, packet->sequence);
					agk::DeleteMemblock(packet->data); //Delete the memblock associated with the packet
					delete packet; //Discard packet if sequence is lower than expected
				}
			}
		}
	}
}

void RUDPListener::ReadACKMessage(int message)
{
	std::string uuid = agk::GetNetworkMessageString(message);
	int sequence = agk::GetNetworkMessageInteger(message);

	auto entry = connectionMap.find(uuid);

	//Connection exists
	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;

		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time

		// O(1) lookup in unordered_map by sequence number
		auto packetIt = connection->outboundPackets.find(sequence);
		if (packetIt != connection->outboundPackets.end())
		{
			agk::DeleteMemblock(packetIt->second->data); // Clean up the memblock
			delete packetIt->second; // Clean up the packet
			connection->outboundPackets.erase(packetIt); // Remove from map
		}
	}
}

void RUDPListener::ReadConnectMessage(int message)
{
	// Extract connection details from the message
	std::string ip = agk::GetNetworkMessageFromIP(message);
	int port = agk::GetNetworkMessageFromPort(message);

	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	
	// Create a new connection and add it to the map
	Connection* connection = new Connection(ip, port);
	connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
	
	if (AddConnection(uuid, connection))
	{
		SendHandshake(connection, 0); // Acknowledge the connection (sequence not used for handshakes)
	}
	else
	{
		delete connection;
	}
}

void RUDPListener::ReadDisconnectMessage(int message)
{
	// Extract connection details from the message
	std::string ip = agk::GetNetworkMessageFromIP(message);
	int port = agk::GetNetworkMessageFromPort(message);

	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	int sequence = agk::GetNetworkMessageInteger(message);

	// Send acknowledgment if connection exists
	auto entry = connectionMap.find(uuid);
	if (entry != connectionMap.end())
	{
		SendAcknowledgment(entry->second, sequence);
	}

	RemoveConnection(uuid);
}

void RUDPListener::ReadHandshakeMessage(int message)
{
	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	auto entry = connectionMap.find(uuid);

	// If the connection already exists, just update it
	if (entry != connectionMap.end())	
	{
		Connection* connection = entry->second;
		// Don't increment inboundSequence - handshakes are control messages, not data
		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
	}
	else
	{
		// If the connection does not exist, create a new one
		std::string ip = agk::GetNetworkMessageFromIP(message);
		int port = agk::GetNetworkMessageFromPort(message);

		Connection* connection = new Connection(ip, port);
		// Don't increment inboundSequence - handshakes are control messages, not data
		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
		AddConnection(uuid, connection);
	}
}

void RUDPListener::ReadHeartbeatMessage(int message)
{
	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	auto entry = connectionMap.find(uuid);

	// If the connection exists, update its last update time
	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;
		// Don't increment inboundSequence - heartbeats are control messages, not data
		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
	}
}

/*
*	Send Functions
*/

void RUDPListener::SendAcknowledgment(Connection* connection, const int sequence) const
{
	// Send an acknowledgment back to the sender
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_ACK);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::AddNetworkMessageInteger(message, sequence);
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
}

void RUDPListener::SendDataMessage(Connection* connection, Packet* packet) const
{
	//Send the packet
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_DATA);
	agk::AddNetworkMessageString(message, packet->uuid.c_str());
	agk::AddNetworkMessageString(message, packet->hash.c_str());
	agk::AddNetworkMessageInteger(message, packet->sequence);
	agk::AddNetworkMessageInteger(message, packet->size);

	// Add fragment metadata if this is a fragment
	agk::AddNetworkMessageByte(message, packet->is_fragment ? 1 : 0);
	if (packet->is_fragment)
	{
		agk::AddNetworkMessageInteger(message, packet->base_sequence);
		agk::AddNetworkMessageInteger(message, packet->fragment_index);
		agk::AddNetworkMessageInteger(message, packet->total_fragments);
		agk::AddNetworkMessageInteger(message, packet->total_size);
		RUDP_DEBUG(("Sending packet fragment "+std::to_string(packet->fragment_index)+"/"+std::to_string(packet->total_fragments) + "\n").c_str());
	}

	// Optimize: Read 4 bytes at a time from memblock to reduce read operations
	int numInts = packet->size / 4;

	for (int i = 0; i < numInts; i++)
	{
		int value = agk::GetMemblockInt(packet->data, i * 4);
		// Add as individual bytes (little-endian)
		agk::AddNetworkMessageByte(message, (value) & 0xFF);
		agk::AddNetworkMessageByte(message, (value >> 8) & 0xFF);
		agk::AddNetworkMessageByte(message, (value >> 16) & 0xFF);
		agk::AddNetworkMessageByte(message, (value >> 24) & 0xFF);
	}

	// Add remaining bytes
	for (int i = numInts * 4; i < packet->size; i++)
	{
		agk::AddNetworkMessageByte(message, agk::GetMemblockByte(packet->data, i));
	}

	//Send the message to the connection
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
	packet->timestamp = std::chrono::steady_clock::now(); //Update timestamp for the next check
}

void RUDPListener::SendHandshake(Connection* connection, int sequence) const
{
	// Send a handshake back to the sender (no sequence needed for control messages)
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_HANDSHAKE);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
}

void RUDPListener::SendHeartbeat(Connection* connection, int sequence) const
{
	// Send a heartbeat to keep connection alive (no sequence needed for control messages)
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_HEARTBEAT);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
}

/*
*	Connection Management
*/

bool RUDPListener::AddConnection(ConnectionUUID uuid, Connection* connection)
{	
	//Add connection only if it doesn't already exist
	if (connectionMap.find(uuid) == connectionMap.end())
	{
		connectionMap.emplace(uuid, connection);
		return true;
	}

	return false;
}

RUDPListener::ConnectionMap::iterator RUDPListener::RemoveConnection(ConnectionUUID uuid)
{
	auto entry = connectionMap.find(uuid);

	if (entry != connectionMap.end())
	{
		delete entry->second; // Delete the connection object
		return connectionMap.erase(entry);
	}

	return connectionMap.end(); // Return end iterator if not found
}

/*
*	Data Parsing
*/

RUDPListener::Packet* RUDPListener::EncodePacket(ConnectionUUID uuid, unsigned int memblock) const
{
	Packet* packet = new Packet();
	packet->uuid = uuid;
	packet->hash = agk::GetMemblockSHA256(memblock); //Calculate the hash of the memblock
	packet->size = agk::GetMemblockSize(memblock);
	packet->data = memblock; //Store the memblock in the packet
	packet->is_fragment = false; // Normal packet, not a fragment
	return packet;
}

RUDPListener::Packet* RUDPListener::EncodeFragmentPacket(ConnectionUUID uuid, unsigned int memblock, int offset, int size, int base_seq, int frag_idx, int total_frags, int total_size) const
{
	Packet* packet = new Packet();
	packet->uuid = uuid;
	packet->size = size;

	// Create a memblock for this fragment only
	unsigned int fragmentMemblock = agk::CreateMemblock(size);

	// Optimize: Copy 4 bytes at a time when aligned
	int numInts = size / 4;

	for (int i = 0; i < numInts; i++)
	{
		int byteOffset = i * 4;
		agk::SetMemblockInt(fragmentMemblock, byteOffset, agk::GetMemblockInt(memblock, offset + byteOffset));
	}

	// Copy remaining bytes
	for (int i = numInts * 4; i < size; i++)
	{
		agk::SetMemblockByte(fragmentMemblock, i, agk::GetMemblockByte(memblock, offset + i));
	}

	packet->hash = agk::GetMemblockSHA256(fragmentMemblock); // Hash of this fragment only
	packet->data = fragmentMemblock;

	// Set fragment metadata
	packet->is_fragment = true;
	packet->base_sequence = base_seq;
	packet->fragment_index = frag_idx;
	packet->total_fragments = total_frags;
	packet->total_size = total_size;

	return packet;
}

void RUDPListener::ReassembleFragments(Connection* connection)
{
	// Check each fragment group for completion
	for (auto it = connection->fragmentMap.begin(); it != connection->fragmentMap.end();)
	{
		int base_seq = it->first;
		PacketList& fragments = it->second;

		// Check if we have all fragments
		if (fragments.empty())
		{
			++it;
			continue;
		}

		int total_fragments = fragments[0]->total_fragments;
		int total_size = fragments[0]->total_size;

		if (static_cast<int>(fragments.size()) == total_fragments)
		{
			// Sort fragments by index
			std::sort(fragments.begin(), fragments.end(),
				[](const Packet* a, const Packet* b) {
					return a->fragment_index < b->fragment_index;
				});

			// Verify all indices are present (0 to total_fragments-1)
			bool all_present = true;
			for (int i = 0; i < total_fragments; i++)
			{
				if (fragments[i]->fragment_index != i)
				{
					all_present = false;
					break;
				}
			}

			if (all_present)
			{
				// Create combined memblock
				unsigned int combinedMemblock = agk::CreateMemblock(total_size);

				int offset = 0;
				for (Packet* fragment : fragments)
				{
					// Copy fragment data into combined memblock
					// Optimize: Copy 4 bytes at a time when aligned
					int numInts = fragment->size / 4;
					int remainingBytes = fragment->size % 4;

					for (int i = 0; i < numInts; i++)
					{
						int byteOffset = i * 4;
						agk::SetMemblockInt(combinedMemblock, offset + byteOffset, agk::GetMemblockInt(fragment->data, byteOffset));
					}

					// Copy remaining bytes
					for (int i = numInts * 4; i < fragment->size; i++)
					{
						agk::SetMemblockByte(combinedMemblock, offset + i, agk::GetMemblockByte(fragment->data, i));
					}

					offset += fragment->size;
				}

				// Create final reassembled packet
				Packet* reassembled = new Packet();
				reassembled->uuid = fragments[0]->uuid;
				reassembled->sequence = base_seq; // Use base_sequence as the packet sequence
				reassembled->size = total_size;
				reassembled->hash = agk::GetMemblockSHA256(combinedMemblock);
				reassembled->data = combinedMemblock;
				reassembled->is_fragment = false;

				// Preserve fragment count so UpdatePendingMessages can increment sequence correctly
				reassembled->total_fragments = total_fragments;

				RUDP_DEBUG(("Reassembled packet created: sequence=" + std::to_string(base_seq) + 
				", total_fragments=" + std::to_string(total_fragments) + 
				", size=" + std::to_string(total_size) + "bytes\n").c_str());

				// Add to pending packets (it will be moved to ready when sequence matches)
				connection->pendingPackets.push_back(reassembled);

				// Clean up fragments
				for (Packet* fragment : fragments)
				{
					agk::DeleteMemblock(fragment->data);
					delete fragment;
				}

				// Remove from fragment map
				it = connection->fragmentMap.erase(it);
				continue;
			}
		}

		++it;
	}
}

RUDPListener::Packet* RUDPListener::DecodeMessage(unsigned int message)
{
	Packet* packet = new Packet();

	// Extract the packet data from the message
	packet->uuid = agk::GetNetworkMessageString(message);
	packet->hash = agk::GetNetworkMessageString(message);
	packet->sequence = agk::GetNetworkMessageInteger(message);
	packet->size = agk::GetNetworkMessageInteger(message); //In bytes

	// Extract fragment metadata
	packet->is_fragment = (agk::GetNetworkMessageByte(message) == 1);
	if (packet->is_fragment)
	{
		packet->base_sequence = agk::GetNetworkMessageInteger(message);
		packet->fragment_index = agk::GetNetworkMessageInteger(message);
		packet->total_fragments = agk::GetNetworkMessageInteger(message);
		packet->total_size = agk::GetNetworkMessageInteger(message);
	}

	unsigned int memblock = agk::CreateMemblock(packet->size);

	// Optimize: Write 4 bytes at a time from network message
	int numInts = packet->size / 4;
	int remainingBytes = packet->size % 4;

	for (int i = 0; i < numInts; i++)
	{
		// Read 4 bytes and combine into integer (little-endian)
		int value = agk::GetNetworkMessageByte(message);
		value |= agk::GetNetworkMessageByte(message) << 8;
		value |= agk::GetNetworkMessageByte(message) << 16;
		value |= agk::GetNetworkMessageByte(message) << 24;
		agk::SetMemblockInt(memblock, i * 4, value);
	}

	// Write remaining bytes
	for (int i = numInts * 4; i < packet->size; i++)
	{
		agk::SetMemblockByte(memblock, i, agk::GetNetworkMessageByte(message));
	}

	packet->data = memblock; //Store the memblock in the packet

	//Return packet if hash is valid
	std::string calculatedHash = agk::GetMemblockSHA256(memblock);
	if(packet->hash == calculatedHash)
		return packet;

	delete packet;
	agk::DeleteMemblock(memblock);	

	return nullptr; //Throw away message if hash doesn't match
}


//Helper function to generate a random UUID string
std::string RUDPListener::GetUUID()
{
	static std::random_device dev;
	static std::mt19937 rng(dev());

	std::uniform_int_distribution<int> dist(0, 15);

	const char* v = "0123456789abcdef";
	const bool dash[] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0 };

	std::string res;
	for (int i = 0; i < 16; i++) {
		if (dash[i]) res += "-";
		res += v[dist(rng)];
		res += v[dist(rng)];
	}
	return res;
}