#ifndef _H_RUDP_LISTENER
#define _H_RUDP_LISTENER

#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class RUDPListener {
	public:
		typedef std::string ConnectionUUID;
		RUDPListener(const std::string address, int port);
		~RUDPListener();
		void Update();
		const std::string GetListenerIP() const;
		int GetListenerPort() const;
		ConnectionUUID GetListenerUUID() const;

		//Settings
		void SetMessageTimeout(std::chrono::milliseconds timeout);
		void SetConnectionTimeout(std::chrono::milliseconds timeout);
		void SetHeartbeatInterval(std::chrono::milliseconds interval);
		void SetMaxThroughputPerFrame(int bytes);

		//Active Connections
		ConnectionUUID GetConnectionUUID(int position) const;
		ConnectionUUID GetConnectionByAddress(const std::string ip, int port) const; //Returns the UUID of the connection by IP and port
		unsigned int GetAGKID() const;
		size_t GetTotalConnections() const;
		const std::string GetConnectionIP(ConnectionUUID uuid) const;
		int GetConnectionPort(ConnectionUUID uuid) const;
		int GetMaxThroughputPerFrame() const;

		//Public Methods
		void Connect(const std::string ip, int port) const;	//Sends connect request won't actually connect until we receive handshake
		void Disconnect(ConnectionUUID uuid);
		void SendMessage(ConnectionUUID uuid, unsigned int memblock);
		int GetMessage(ConnectionUUID uuid);
		size_t GetMessageCount(ConnectionUUID uuid);

		

	private:
		//Message header
		typedef enum MessageType {			
			MSG_CONNECT,	// Connection message
			MSG_DISCONNECT,	// Disconnection message
			MSG_HANDSHAKE,	// Received after a connection message
			MSG_HEARTBEAT,	// Keep-alive message
			MSG_DATA,		// Data message
			MSG_ACK,		// Acknowledgment message
		} MessageType;

		typedef struct Packet {
			ConnectionUUID uuid;	//UUID of the connection
			int sequence;
			int size;
			std::string hash;
			unsigned int data;
			std::chrono::steady_clock::time_point timestamp;

			// Fragment fields
			bool is_fragment = false;
			int base_sequence = -1;      // Base sequence ID for the fragmented message group
			int fragment_index = -1;     // Which fragment (0, 1, 2...)
			int total_fragments = -1;    // Total number of fragments in the group
			int total_size = -1;         // Original message size before fragmentation
		} Packet;

		typedef std::vector<Packet*> PacketList;
		typedef std::unordered_map<int, Packet*> PacketMap; // Map of sequence -> packet for fast ACK lookup

		typedef struct Connection {
			const std::string ip;
			int port;
			int inboundSequence;
			int outboundSequence;

			std::chrono::steady_clock::time_point lastUpdate;
			std::chrono::steady_clock::time_point lastHeartbeatSent;

			PacketList readyPackets;	//Packets ready to be read
			PacketList pendingPackets;	//Packets waiting for earlier messages
			PacketMap outboundPackets;	//Map of sequence -> packet for fast ACK lookup
			PacketList queuedOutbound;	//Packets queued to send (bandwidth limited)
			std::map<int, PacketList> fragmentMap;	//Map of base_sequence -> list of fragments

			int sendBudgetThisFrame; // Bytes remaining to send this frame (64KB limit)

			Connection(const std::string ip, int port)
				: ip(ip), port(port), inboundSequence(0), outboundSequence(0), sendBudgetThisFrame(65536) 
			{
				lastUpdate = std::chrono::steady_clock::now();
				lastHeartbeatSent = std::chrono::steady_clock::now();
			}
			~Connection() 
			{
				for (Packet* packet : readyPackets) 
				{
					delete packet;
				}

				for (Packet* packet : pendingPackets)
				{
					delete packet;
				}

				for (auto& pair : outboundPackets)
				{
					delete pair.second;
				}

				for (Packet* packet : queuedOutbound)
				{
					delete packet;
				}

				// Clean up fragmentMap
				for (auto& fragmentEntry : fragmentMap)
				{
					for (Packet* packet : fragmentEntry.second)
					{
						delete packet;
					}
				}
			}
		}Connection;
		
		typedef std::map<ConnectionUUID, Connection*> ConnectionMap;



		//Listener variables
		ConnectionMap connectionMap;	//Map of active connections
		ConnectionUUID listenerUUID;	//Our UUID
		std::string address;
		int port;
		unsigned int AGKListener;

		std::chrono::milliseconds HEARTBEAT_INTERVAL = std::chrono::milliseconds(50); //How often to send heartbeat messages
		std::chrono::milliseconds OUTBOUND_TIMEOUT = std::chrono::milliseconds(500); //Timeout for outbound packets
		std::chrono::milliseconds CONNECTION_TIMEOUT = std::chrono::milliseconds(10000); //Timeout for connection
		int MAX_SEND_PER_FRAME = 65536; // 64KB bandwidth limit per frame
		
		//Updates
		void ReadIncomingMessages();
		void UpdatePendingMessages();
		void ProcessOutboundQueue(); // Process queued packets with bandwidth limiting
		void CheckOutgoingMessages();
		void CheckTimeout();
		void SendHeartbeats();

		//Read Messages by header type
		void ReadDataMessage(int message);
		void ReadACKMessage(int message);
		void ReadConnectMessage(int message);
		void ReadDisconnectMessage(int message);
		void ReadHandshakeMessage(int message);
		void ReadHeartbeatMessage(int message);

		//Send functions
		void SendAcknowledgment(Connection* connection, int sequence) const;
		void SendDataMessage(Connection* connection, Packet* packet) const;
		void SendHandshake(Connection* connection, int sequence) const;
		void SendHeartbeat(Connection* connection, int sequence) const;

		//Connection management
		bool AddConnection(ConnectionUUID uuid, Connection* connection);
		ConnectionMap::iterator RemoveConnection(ConnectionUUID uuid);

		//Data Parsing
		Packet* DecodeMessage(unsigned int message);
		Packet* EncodePacket(ConnectionUUID uuid, unsigned int memblock) const;
		Packet* EncodeFragmentPacket(ConnectionUUID uuid, unsigned int memblock, int offset, int size, int base_seq, int frag_idx, int total_frags, int total_size) const;

		//Fragment handling
		void ReassembleFragments(Connection* connection);

		//UUID Helper
		std::string RUDPListener::GetUUID();


		};

#endif 
