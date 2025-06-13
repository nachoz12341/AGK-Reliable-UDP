// Includes
#include "template.h"
#include <string>

// Namespace
using namespace AGK;

app App;
RUDPListener* rListener;
std::string destinationIP;

void app::Begin(void)
{
	agk::SetVirtualResolution (1024, 768);
	agk::SetClearColor( 151,170,204 ); // light blue
	agk::SetSyncRate(60,0);
	agk::SetScissor(0,0,0,0);

	rListener = new RUDPListener("anyip4", 30000);
	destinationIP = "192.168.1.8";	//Fill in with your destination IP
}

int app::Loop (void)
{
	std::string connectMessage = "Press space to connect to " + destinationIP;
	std::string activeMessage = "Active Connections: " + std::to_string(rListener->GetTotalConnections());

	agk::Print(connectMessage.c_str());
	agk::Print(activeMessage.c_str());

	if (rListener->GetTotalConnections() > 0)
	{
		//Send heartbeat to all connections
		for (int i = 0; i < rListener->GetTotalConnections();i++)
		{
			RUDPListener::ConnectionUUID uuid = rListener->GetConnectionUUID(i);
			SendHeartbeat(uuid);
			ReadMessages(uuid);

			if (agk::GetRawKeyPressed(32))	//Try to connect if you press space
			{
				rListener->Disconnect(uuid); // Disconnect the connection
			}
		}
	}
	else
	{
		if (agk::GetRawKeyPressed(32))	//Try to connect if you press space
		{
			rListener->Connect(destinationIP.c_str(), 30000); // Connect to the listener
		}
	}

	rListener->Update();	//Call every frame
	agk::Sync();
	return 0; // return 1 to close app
}


void app::End (void)
{
	delete rListener; // Clean up the RUDPListener
}


void app::SendHeartbeat(RUDPListener::ConnectionUUID uuid)
{
	unsigned int memblock = agk::CreateMemblock(4);
	agk::SetMemblockInt(memblock, 0, 1);
	rListener->SendMessage(uuid, memblock);
}

void app::ReadMessages(RUDPListener::ConnectionUUID uuid)
{
	int message = rListener->GetMessage(uuid);

	while (message != -1)
	{
		std::string hb_msg = "Received heartbeat from connection: " + std::string(uuid);
		agk::Print(hb_msg.c_str());
		agk::DeleteMemblock(message);	//Clean up
		message = rListener->GetMessage(uuid);
	}
}