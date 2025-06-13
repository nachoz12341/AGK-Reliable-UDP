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

		//Settings
		void SetMessageTimeout(std::chrono::milliseconds timeout);
		void SetConnectionTimeout(std::chrono::milliseconds timeout);

		//Active Connections
		ConnectionUUID GetConnectionUUID(int position) const;
		size_t GetTotalConnections() const;

		//Public Methods
		void Connect(const char* ip, int port) const;	//Sends connect request won't actually connect until we receive handshake
		void Disconnect(ConnectionUUID uuid);
		void SendMessage(ConnectionUUID uuid, unsigned int memblock);
		int GetMessage(ConnectionUUID uuid);

		

	private:
		//Message header
		typedef enum MessageType {
			MSG_DATA,	//Data message
			MSG_ACK,	//Acknowledgment message
			MSG_CONNECT,	//Connection message
			MSG_DISCONNECT,	//Disconnection message
			MSG_HANDSHAKE
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

		std::chrono::milliseconds OUTBOUND_TIMEOUT = std::chrono::milliseconds(100); //Timeout for outbound packets
		std::chrono::milliseconds CONNECTION_TIMEOUT = std::chrono::milliseconds(10000); //Timeout for connection
		
		//Updates
		void ReadIncomingMessages();
		void UpdatePendingMessages();
		void CheckOutgoingMessages();
		void CheckTimeout();

		//Read Messages by header type
		void ReadDataMessage(int message);
		void ReadACKMessage(int message);
		void ReadConnectMessage(int message);
		void ReadDisconnectMessage(int message);
		void ReadHandshakeMessage(int message);

		//Send functions
		void SendAcknowledgment(Connection* connection, int sequence) const;
		void SendDataMessage(Connection* connection, Packet* packet) const;
		void SendHandshake(Connection* connection, int sequence) const;

		//Connection management
		bool AddConnection(ConnectionUUID uuid, Connection* connection);
		ConnectionMap::iterator RemoveConnection(ConnectionUUID uuid);

		//Data Parsing
		Packet* DecodeMessage(unsigned int message);
		Packet* EncodePacket(ConnectionUUID uuid, unsigned int memblock) const;
		
		
};

#endif 
