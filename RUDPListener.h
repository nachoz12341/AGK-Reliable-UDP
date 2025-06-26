#ifndef _H_RUDP_LISTENER
#define _H_RUDP_LISTENER

#include <chrono>
#include <map>
#include <vector>

class RUDPListener {
	public:
		typedef char* ConnectionUUID;
		RUDPListener(const char* address, int port);
		~RUDPListener();
		void Update();
		const char* GetListenerIP() const;
		int GetListenerPort() const;

		//Settings
		void SetMessageTimeout(std::chrono::milliseconds timeout);
		void SetConnectionTimeout(std::chrono::milliseconds timeout);
		void SetHeartbeatInterval(std::chrono::milliseconds interval);

		//Active Connections
		ConnectionUUID GetConnectionUUID(int position) const;
		ConnectionUUID GetConnectionByAddress(const char* ip, int port) const; //Returns the UUID of the connection by IP and port
		unsigned int GetConnectionID() const;
		size_t GetTotalConnections() const;
		const char* GetConnectionIP(ConnectionUUID uuid) const;
		int GetConnectionPort(ConnectionUUID uuid) const;


		//Public Methods
		void Connect(const char* ip, int port) const;	//Sends connect request won't actually connect until we receive handshake
		void Disconnect(ConnectionUUID uuid);
		void SendMessage(ConnectionUUID uuid, unsigned int memblock);
		int GetMessage(ConnectionUUID uuid);

		

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
			const char* hash;
			unsigned int data;
			std::chrono::steady_clock::time_point timestamp;
		} Packet;

		typedef std::vector<Packet*> PacketList;

		typedef struct Connection {
			const char* ip;
			int port;
			int inboundSequence;
			int outboundSequence;

			std::chrono::steady_clock::time_point lastUpdate;

			PacketList readyPackets;	//Packets ready to be read
			PacketList pendingPackets;	//Packets waiting for earlier messages
			PacketList outboundPackets;	//Packets we've sent to this client

			Connection(const char* ip, int port)
				: ip(ip), port(port), inboundSequence(0), outboundSequence(0) {}
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

				for (Packet* packet : outboundPackets)
				{
					delete packet;
				}
			}
		}Connection;

		//Key's being a pointer need to be compared by value
		struct cmp_str {
			bool operator()(const char* a, const char* b) const {
				return std::strcmp(a, b) < 0;
			}
		};
		
		typedef std::map<ConnectionUUID, Connection*, cmp_str> ConnectionMap;



		//Listener variables
		ConnectionMap connectionMap;	//Map of active connections
		ConnectionUUID listenerUUID;	//Our UUID
		char* address;
		int port;
		unsigned int AGKListener;

		std::chrono::milliseconds HEARTBEAT_INTERVAL = std::chrono::milliseconds(16); //How often to send heartbeat messages
		std::chrono::milliseconds OUTBOUND_TIMEOUT = std::chrono::milliseconds(100); //Timeout for outbound packets
		std::chrono::milliseconds CONNECTION_TIMEOUT = std::chrono::milliseconds(10000); //Timeout for connection
		
		//Updates
		void ReadIncomingMessages();
		void UpdatePendingMessages();
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
		
		
};

#endif 
