/*
** wl_net.cpp
**
**---------------------------------------------------------------------------
** Copyright 2014 Braden Obrzut
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include <SDL.h>
#include <SDL_net.h>

#include "wl_def.h"
#include "id_in.h"
#include "id_us.h"
#include "id_vh.h"
#include "wl_game.h"
#include "wl_menu.h"
#include "wl_play.h"
#include "wl_net.h"
#include "m_swap.h"
#include "m_random.h"
#include "doomerrors.h"
#include "zdoomsupport.h"
#include "zstring.h"

#include "v_video.h"

#define NET_DEFAULT_PORT 5029
#define MAXEXTRATICS 4

// TODO: Handle transfer of arbiter status as client quit
#define Arbiter 0

namespace Net {

enum
{
	NET_RequestConnection,
	NET_ConnectionStart,
	NET_Ack,
	NET_TicCmd,
	NET_NewGame
};

#pragma pack(1)
struct RequestPacket
{
	// This could be static const BYTE, but I know old Mac compilers I use
	// sometimes have problems with that. :(
	enum { Type = NET_RequestConnection };

	BYTE type;
};

struct StartPacket
{
	enum { Type = NET_ConnectionStart };

	BYTE type;
	BYTE playerNumber;
	BYTE numPlayers;
	DWORD rngseed;
	struct Client
	{
		DWORD host;
		WORD port;
	} clients[];
};

struct NewGamePacket
{
	enum { Type = NET_NewGame };

	BYTE type;
	int32_t TimeCount;
	int32_t playerClass;
	BYTE difficulty;
	char map[9];
};

struct AckPacket
{
	enum { Type = NET_Ack };

	BYTE type;
	BYTE ackedType;
	int32_t TimeCount;
};

struct TicCmdPacket
{
	enum { Type = NET_TicCmd };

	BYTE type;
	int32_t TimeCount;
	int controlx;
	int controly;
	int controlstrafe;
	bool buttonstate[NUMBUTTONS];
	bool buttonheld[NUMBUTTONS];
};
#pragma pack()

NetInit InitVars = {
	MODE_SinglePlayer,
	NET_DEFAULT_PORT,
	1
};

struct NetClient
{
	IPaddress address;
	TicCmdPacket extratics[MAXEXTRATICS];
	unsigned short extrapos;
};

static NetClient Client[MAXPLAYERS];
static UDPsocket Socket;
static UDPpacket *Packet;

// Just so that we know something is happening do a little animation.
static const char* const Waiting[4] = {"   ", ".  ", ".. ", "..." };

static FString IPaddressToString(IPaddress address)
{
	FString out;
	out.Format("%u.%u.%u.%u:%d", address.host&0xFF, (address.host&0xFF00)>>8, (address.host&0xFF0000)>>16, (address.host&0xFF000000)>>24, BigShort(address.port));
	return out;
}

static FString BuildClientList(bool acked[MAXPLAYERS])
{
	FString clients;

	for(unsigned i = 1;i < InitVars.numPlayers;++i)
	{
		FString client;
		if(Client[i].address.host)
			client.Format("%2u: %-21s %-6s", i, IPaddressToString(Client[i].address).GetChars(), acked[i] ? "Synced" : "");
		else
			client.Format("%2u: %-21s %-6s", i, "Waiting...", "");

		clients += FString(i != 1 ? "\n": "") + client;
	}
	return clients;
}

// Returns the player number for a given ip address
static int FindClient(IPaddress address)
{
	for(unsigned int i = 0;i < InitVars.numPlayers;++i)
	{
		if(Client[i].address.host == address.host && Client[i].address.port == address.port)
			return i;
	}
	return -1;
}

// Check if we have a potentially valid packet of a certain type
template<typename T>
static bool CheckPacketType(const UDPpacket *packet)
{
	return packet->len >= (signed)sizeof(T) && ((T*)packet->data)->type == T::Type;
}

// Sends an ACK packet to a given address
template<typename T>
static void SendAck(IPaddress address, int32_t TimeCount)
{
	AckPacket ackData;
	ackData.type = AckPacket::Type;
	ackData.ackedType = T::Type;
	ackData.TimeCount = TimeCount;
	UDPpacket packet = { -1, (Uint8*)&ackData, sizeof(AckPacket), sizeof(AckPacket), 0, address };

	SDLNet_UDP_Send(Socket, -1, &packet);
}

template<typename T>
bool BufferPacket(int client, const T &packet)
{
	return false;
}

template<>
bool BufferPacket<TicCmdPacket>(int client, const TicCmdPacket &packet)
{
	if(packet.TimeCount > gamestate.TimeCount)
	{
		Client[client].extratics[Client[client].extrapos] = packet;
		Client[client].extrapos = (Client[client].extrapos+1)%MAXEXTRATICS;
	}
	return true;
}

template<typename T>
int UnbufferPacket(T (&packets)[MAXPLAYERS], bool (&received)[MAXPLAYERS])
{
	return 0;
}

template<>
int UnbufferPacket<TicCmdPacket>(TicCmdPacket (&packets)[MAXPLAYERS], bool (&received)[MAXPLAYERS])
{
	int unbufferedCount = 0;
	for(unsigned int i = 0;i < MAXEXTRATICS;++i)
	{
		for(unsigned int c = 0;c < InitVars.numPlayers;++c)
		{
			if(c == ConsolePlayer)
				continue;

			if(Client[c].extratics[i].TimeCount == gamestate.TimeCount)
			{
				packets[c] = Client[c].extratics[i];
				if(received[c])
					continue;
				received[c] = true;
				++unbufferedCount;
			}
		}
	}
	return unbufferedCount;
}

// Synchronously exchange a packet to all players and wait for ACK
template<typename T>
static void ExchangePacket(T (&packets)[MAXPLAYERS])
{
	bool acked[MAXPLAYERS] = { false };
	bool received[MAXPLAYERS] = { false };
	int numAcked = 1, numReceived = 1;
	acked[ConsolePlayer] = true;
	received[ConsolePlayer] = true;

	numReceived += UnbufferPacket(packets, received);

	UDPpacket outPacket = { -1, (Uint8*)&packets[ConsolePlayer], sizeof(T), sizeof(T), 0 };
	packets[ConsolePlayer].type = T::Type;
	packets[ConsolePlayer].TimeCount = gamestate.TimeCount;

	// We need to keep an eye out for packets, but we also need to periodically
	// resend our ticcmd in case it got lost.
	unsigned int resend = 0;
	while(numAcked != InitVars.numPlayers || numReceived != InitVars.numPlayers)
	{
		if(resend == 0)
		{
			for(unsigned int i = 0;i < InitVars.numPlayers;++i)
			{
				if(acked[i])
					continue;

				outPacket.address = Client[i].address;
				SDLNet_UDP_Send(Socket, -1, &outPacket);
			}
			resend = 100;
		}

		--resend;
		SDL_Delay(1);
		IN_ProcessEvents();
		while(SDLNet_UDP_Recv(Socket, Packet))
		{
			if(CheckPacketType<T>(Packet))
			{
				int client = FindClient(Packet->address);
				if(client < 0)
				{
					Printf("Packet recieved from unknown source\n");
					continue;
				}

				packets[client] = *reinterpret_cast<T *>(Packet->data);
				T &data = packets[client];

				if(data.TimeCount != gamestate.TimeCount)
				{
					if(BufferPacket<T>(client, packets[client]))
						SendAck<T>(Packet->address, data.TimeCount);
					continue;
				}

				SendAck<T>(Packet->address, data.TimeCount);

				if(received[client])
					continue;
				received[client] = true;
				++numReceived;
			}
			else if(CheckPacketType<AckPacket>(Packet))
			{
				const AckPacket *data = reinterpret_cast<AckPacket *>(Packet->data);
				if(data->TimeCount != gamestate.TimeCount || data->ackedType != T::Type)
					continue;

				int client = FindClient(Packet->address);
				if(client < 0)
				{
					Printf("Packet recieved from unknown source\n");
					continue;
				}
				if(acked[client])
					continue;

				acked[client] = true;
				++numAcked;
			}
			else if(CheckPacketType<StartPacket>(Packet))
			{
				// Host lost our start ack, so send another one
				SendAck<StartPacket>(Client[0].address, 0xFFFFFFFF);
			}
		}
	}
}

static void StartHost(InitStatusCallback callback)
{
	unsigned int waitpos = 0;
	unsigned int nextclient = 1; // 0 is the host
	bool acked[MAXPLAYERS] = { false };

	if(!(Socket = SDLNet_UDP_Open(InitVars.port)))
		throw CFatalError("Could not open UDP socket.");

	// Step 1: Get connection requests from each player
	Printf("Waiting for %d players:\n   ", InitVars.numPlayers);
	callback(BuildClientList(acked));
	while(nextclient != InitVars.numPlayers)
	{
		waitpos = (waitpos+1)%4;
		Printf("\b\b\b%s", Waiting[waitpos]);
		fflush(stdout);

		if(SDLNet_UDP_Recv(Socket, Packet))
		{
			const RequestPacket *data = reinterpret_cast<RequestPacket*>(Packet->data);
			if(CheckPacketType<RequestPacket>(Packet))
			{
				Printf("\b\b\b");

				int client = FindClient(Packet->address);
				if(client == -1)
				{
					Printf("[%d] New connection from %u.%u.%u.%u:%u!\n", nextclient, Packet->address.host&0xFF, (Packet->address.host&0xFF00)>>8, (Packet->address.host&0xFF0000)>>16, (Packet->address.host&0xFF000000)>>24, BigShort(Packet->address.port));
					Client[nextclient++].address = Packet->address;
					callback(BuildClientList(acked));
				}

				Printf("   ");
			}
		}
		else
		{
			SDL_Delay(100);
			IN_ProcessEvents();
		}
	}

	// Once we found all of the players, send a syncronization packet which
	// contains anything needs to start the game as well as the address of each
	// other player in the game (they already know the host).
	Printf("\b\b\bAll players connected! Sending sync...\n");

	const int startSize = sizeof(StartPacket) + sizeof(StartPacket::Client)*(InitVars.numPlayers-1);
	StartPacket *startData = (StartPacket *)malloc(startSize);
	UDPpacket startPacket = { -1, (Uint8*)startData, startSize, startSize, 0 };
	startData->type = StartPacket::Type;
	startData->numPlayers = InitVars.numPlayers;
	startData->rngseed = rngseed;
	for(unsigned int i = 1;i < InitVars.numPlayers;++i)
	{
		startData->clients[i-1].host = Client[i].address.host;
		startData->clients[i-1].port = Client[i].address.port;
	}

	nextclient = 1;
	while(nextclient != InitVars.numPlayers)
	{
		// Send off start packet to players who have not acked
		for(unsigned int i = 1;i < InitVars.numPlayers;++i)
		{
			if(acked[i])
				continue;

			startPacket.address = Client[i].address;
			startData->playerNumber = i;

			SDLNet_UDP_Send(Socket, -1, &startPacket);
		}

		SDL_Delay(100);
		IN_ProcessEvents();

		// Look for ack packets
		while(SDLNet_UDP_Recv(Socket, Packet))
		{
			const AckPacket *data = reinterpret_cast<AckPacket*>(Packet->data);
			if(CheckPacketType<AckPacket>(Packet))
			{
				int client = FindClient(Packet->address);
				if(client > 0)
				{
					acked[client] = true;
					++nextclient;
					callback(BuildClientList(acked));
				}
			}
		}
	}

	free(startData);

	Printf("All acked starting game!\n");
}

static void StartJoin(InitStatusCallback callback)
{
	unsigned int waitpos = 0;
	if(!(Socket = SDLNet_UDP_Open(InitVars.port)))
		throw CFatalError("Could not open UDP socket.");
	IPaddress address;

	// Convert join string to IPaddress
	FString addrString(InitVars.joinAddress);
	uint16_t port = NET_DEFAULT_PORT;
	if(addrString.IndexOf(':') != -1)
	{
		long pos = addrString.IndexOf(':');
		port = atoi(addrString.Mid(pos+1));
		addrString = addrString.Left(pos);
	}
	SDLNet_ResolveHost(&address, addrString, port);

	Printf("Attempting to connect to %u.%u.%u.%u:%u :\n   ", address.host&0xFF, (address.host&0xFF00)>>8, (address.host&0xFF0000)>>16, (address.host&0xFF000000)>>24, BigShort(address.port));

	// Send a connection request to host
	Uint8 requestData[] = {NET_RequestConnection};
	UDPpacket packet = { -1, requestData, 1, 1, 0, address };

	callback("Waiting for sync");
	for(;;)
	{
		waitpos = (waitpos+1)%4;
		Printf("\b\b\b%s", Waiting[waitpos]);
		fflush(stdout);

		// Send request periodically as a heart beat
		if(waitpos == 0)
		{
			SDLNet_UDP_Send(Socket, -1, &packet);
		}

		// Look for start sync packets
		if(SDLNet_UDP_Recv(Socket, Packet))
		{
			const StartPacket *data = reinterpret_cast<StartPacket *>(Packet->data);
			if(CheckPacketType<StartPacket>(Packet))
			{
				ConsolePlayer = data->playerNumber;
				InitVars.numPlayers = data->numPlayers;
				rngseed = data->rngseed;

				Client[0].address = Packet->address;
				for(unsigned int i = 1;i < InitVars.numPlayers;++i)
				{
					Client[i].address.host = data->clients[i-1].host;
					Client[i].address.port = data->clients[i-1].port;
				}
				break;
			}
		}
		else
		{
			SDL_Delay(100);
			IN_ProcessEvents();
		}
	}

	Printf("\b\b\bRecieved sync from host! Sending ack...\n");
	callback("Sync recieved");

	// Send ACK and forget, if we're waiting for ticcmd and we get a start, we'll send another ack then.
	SendAck<StartPacket>(address, 0xFFFFFFFF);
}

static void Shutdown()
{
	SDLNet_FreePacket(Packet);
	SDLNet_UDP_Close(Socket);
}

// Returns true when network init finished
void Init(InitStatusCallback callback)
{
	if(InitVars.mode == MODE_SinglePlayer)
		return;

	if(SDLNet_Init() < 0)
	{
		I_FatalError("Unable to init SDL_net: %s", SDLNet_GetError());
	}

	Packet = SDLNet_AllocPacket(1500);
	atterm(Shutdown);

	if(InitVars.mode == MODE_Host)
		StartHost(callback);
	else
		StartJoin(callback);
}

bool IsArbiter()
{
	return ConsolePlayer == Arbiter;
}

void NewGame(int &difficulty, FString &map, FName (&playerClassNames)[MAXPLAYERS])
{
	if(InitVars.numPlayers > 1)
	{
		WindowX = WindowY = 0;
		WindowW = 320;
		WindowH = 200;
		Message("Waiting for all players to start");
	}

	NewGamePacket newGamePackets[MAXPLAYERS];
	NewGamePacket &myNewGameRequest = newGamePackets[ConsolePlayer];

	myNewGameRequest.difficulty = difficulty;
	myNewGameRequest.playerClass = playerClassNames[ConsolePlayer];
	strncpy(myNewGameRequest.map, map, 8);
	myNewGameRequest.map[8] = 0;

	ExchangePacket(newGamePackets);
	for(unsigned int client = 0;client < InitVars.numPlayers;++client)
	{
		playerClassNames[client] = FName(static_cast<ENamedName>(newGamePackets[client].playerClass));

		if(client == Arbiter)
		{
			difficulty = newGamePackets[client].difficulty;
			newGamePackets[client].map[8] = 0;
			map = newGamePackets[client].map;
		}
	}
}

void PollControls()
{
	TicCmdPacket ticcmdPackets[MAXPLAYERS];
	bool controls[MAXPLAYERS] = { false };

	// We need to send a ticcmd to each player in the game.
	TicCmdPacket &ticcmdData = ticcmdPackets[ConsolePlayer];
	ticcmdData.controlx = control[ConsolePlayer].controlx;
	ticcmdData.controly = control[ConsolePlayer].controly;
	ticcmdData.controlstrafe = control[ConsolePlayer].controlstrafe;
	memcpy(ticcmdData.buttonstate, control[ConsolePlayer].buttonstate, sizeof(control[ConsolePlayer].buttonstate));
	memcpy(ticcmdData.buttonheld, control[ConsolePlayer].buttonheld, sizeof(control[ConsolePlayer].buttonheld));

	ExchangePacket(ticcmdPackets);
	for(unsigned int client = 0;client < InitVars.numPlayers;++client)
	{
		if(client == ConsolePlayer)
			continue;

		TicCmdPacket &data = ticcmdPackets[client];
		control[client].controlx = data.controlx;
		control[client].controly = data.controly;
		control[client].controlstrafe = data.controlstrafe;
		memcpy(control[client].buttonstate, data.buttonstate, sizeof(control[client].buttonstate));
		memcpy(control[client].buttonheld, data.buttonheld, sizeof(control[client].buttonheld));
	}
}

}
