#include "RUDPListener.h"
#include <algorithm>	// For sort
#include "agk.h"		// For AGK functions
#include "Util.h"		// For get_uuid function


RUDPListener::RUDPListener(const std::string address, int port)
{
	// Initialize the listener with the given address and port
	this->address = address;
	this->port = port; 

	this->listenerUUID = get_uuid();

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
		}

		for (Packet* packet : entry.second->pendingPackets)
		{
			agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
		}

		for (Packet* packet : entry.second->outboundPackets)
		{
			agk::DeleteMemblock(packet->data); // Delete the memblock associated with the packet
		}

		//Connection struct destroy destroys remaining references
		delete entry.second;	
	}

	agk::DeleteUDPListener(AGKListener);
}

//Needs to be called every frame
void RUDPListener::Update()
{
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

		//Parse packet
		Packet* packet = EncodePacket(listenerUUID, memblock);
		packet->sequence = connection->outboundSequence++;

		//Send packet
		SendDataMessage(connection, packet);

		//Add to outbound
		connection->outboundPackets.push_back(packet);
	}
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
					connection->inboundSequence++;
					connection->readyPackets.push_back(packet);
					it = connection->pendingPackets.erase(it); // Remove from pending
				}
				else
				{
					it++; // Move to the next packet
				}
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

		// If there are packets we have not acknowledged yet
		for (Packet* packet : connection->outboundPackets)
		{
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
			SendHeartbeat(connection, connection->outboundSequence++);
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

			//Check if the sequence is valid
			if (packet->sequence == connection->inboundSequence)
			{
				connection->inboundSequence++; //Increment sequence for next message
				connection->readyPackets.push_back(packet); //Add packet to ready packets
				SendAcknowledgment(connection, packet->sequence);
			}
			else if (packet->sequence > connection->inboundSequence)
			{
				connection->pendingPackets.push_back(packet); //Add packet to ready packets
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

		//Search for a packet matching the sequence number
		for (int i = 0;i < connection->outboundPackets.size();i++)
		{
			if (connection->outboundPackets[i]->sequence == sequence)
			{
				connection->outboundPackets.erase(connection->outboundPackets.begin() + i); //Remove the packet from outbound packets
				return;
			}
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
		SendHandshake(connection, connection->outboundSequence++); // Acknowledge the connection
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

	// Send acknowledgment and remove the connection
	Connection* connection = new Connection(ip, port);
	connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
	SendAcknowledgment(connection, sequence); 
	RemoveConnection(uuid);
}

void RUDPListener::ReadHandshakeMessage(int message)
{
	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	int sequence = agk::GetNetworkMessageInteger(message);
	auto entry = connectionMap.find(uuid);

	// If the connection already exists, just update it
	if (entry != connectionMap.end())	
	{
		Connection* connection = entry->second;
		connection->inboundSequence++; //Increment sequence for next message
		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
	}
	else
	{
		// If the connection does not exist, create a new one
		std::string ip = agk::GetNetworkMessageFromIP(message);
		int port = agk::GetNetworkMessageFromPort(message);

		Connection* connection = new Connection(ip, port);
		connection->inboundSequence++; //Increment sequence for next message
		connection->lastUpdate = std::chrono::steady_clock::now(); //Update last update time
		AddConnection(uuid, connection);
	}
}

void RUDPListener::ReadHeartbeatMessage(int message)
{
	ConnectionUUID uuid = agk::GetNetworkMessageString(message);
	int sequence = agk::GetNetworkMessageInteger(message);
	auto entry = connectionMap.find(uuid);

	// If the connection exists, update its last update time
	if (entry != connectionMap.end())
	{
		Connection* connection = entry->second;
		connection->inboundSequence++; //Increment sequence for next message
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
	//Resend the packet
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_DATA);
	agk::AddNetworkMessageString(message, packet->uuid.c_str());
	agk::AddNetworkMessageString(message, packet->hash.c_str());
	agk::AddNetworkMessageInteger(message, packet->sequence);
	agk::AddNetworkMessageInteger(message, packet->size);

	for (int i = 0; i < packet->size; i++)
	{
		agk::AddNetworkMessageByte(message, agk::GetMemblockByte(packet->data, i));
	}
	//Send the message to the connection
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
	packet->timestamp = std::chrono::steady_clock::now(); //Update timestamp for the next check
}

void RUDPListener::SendHandshake(Connection* connection, int sequence) const
{
	// Send an acknowledgment back to the sender
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_HANDSHAKE);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::AddNetworkMessageInteger(message, sequence);
	agk::SendUDPNetworkMessage(AGKListener, message, connection->ip.c_str(), connection->port);
}

void RUDPListener::SendHeartbeat(Connection* connection, int sequence) const
{
	// Send an acknowledgment back to the sender
	unsigned int message = agk::CreateNetworkMessage();
	agk::AddNetworkMessageByte(message, MessageType::MSG_HEARTBEAT);
	agk::AddNetworkMessageString(message, listenerUUID.c_str());
	agk::AddNetworkMessageInteger(message, sequence);
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
	return packet;
}

RUDPListener::Packet* RUDPListener::DecodeMessage(unsigned int message)
{
	Packet* packet = new Packet();

	// Extract the packet data from the message
	packet->uuid = agk::GetNetworkMessageString(message);
	packet->hash = agk::GetNetworkMessageString(message);
	packet->sequence = agk::GetNetworkMessageInteger(message);
	packet->size = agk::GetNetworkMessageInteger(message); //In bytes

	unsigned int memblock = agk::CreateMemblock(packet->size);

	for (int i = 0;i < packet->size ;i++)
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
