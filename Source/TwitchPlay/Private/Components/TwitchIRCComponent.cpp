// Copyright (C) Simone Di Gravio <email: altairjp@gmail.com> - All Rights Reserved

#include "Components/TwitchIRCComponent.h"

FTwitchMessageReceiver::FTwitchMessageReceiver()
	: SendingQueue(MakeUnique<FTwitchSendMessagesQueue>())
	, ReceivingQueue(MakeUnique<FTwitchReceiveMessagesQueue>())
	, ConnectionQueue(MakeUnique<FTwitchConnectionQueue>())
	, ConnectionSocket(nullptr)
	, MessagesThread(nullptr)
	, ShouldExit(false)
	, WaitingForAuth(false)
{
	
}

FTwitchMessageReceiver::~FTwitchMessageReceiver()
{
	if (ConnectionSocket != nullptr)
	{
		ConnectionSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket);
		ConnectionSocket = nullptr;
	}

	SendingQueue = nullptr;
	ReceivingQueue = nullptr;
	ConnectionQueue = nullptr;
	MessagesThread = nullptr;
}

void FTwitchMessageReceiver::StartConnection(const FString& oauth, const FString& username, const FString& channel)
{
	checkf(!MessagesThread, TEXT("FTwitchMessageReceiver::StartConnection called more than once?"));
	Oauth = oauth;
	Username = username.ToLower();
	Channel = channel.ToLower();
	MessagesThread = FRunnableThread::Create(this, TEXT("FTwitchMessageReceiver"));
}

inline FString ANSIBytesToString(const uint8* In, int32 Count)
{
	FString Result;
	Result.Empty(Count);

	while (Count)
	{
		Result += static_cast<ANSICHAR>(*In);

		++In;
		Count--;
	}
	return Result;
}

uint32 FTwitchMessageReceiver::Run()
{
	if(!ConnectionSocket)
	{
		// Create the server connection
		ISocketSubsystem* sss = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> connection_addr = sss->CreateInternetAddr();

		FAddressInfoResult GAIResult = sss->GetAddressInfo(TEXT("irc.twitch.tv"),
											     nullptr,
											     EAddressInfoFlags::Default,
											     NAME_None);
		if (GAIResult.Results.Num() == 0)
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_CONNECT, TEXT("Could not resolve hostname!")));
			return 1; // if the host could not be resolved return false
		}

		connection_addr->SetRawIp(GAIResult.Results[0].Address->GetRawIp());

		// Set connection port
		const int32 port = 6667; // Standard IRC port
		connection_addr->SetPort(port);

		FSocket* ret_socket = sss->CreateSocket(NAME_Stream, TEXT("TwitchPlay Socket"), false);

		// Socket creation might fail on certain subsystems
		if (ret_socket == nullptr)
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_CONNECT, TEXT("Could not create socket!")));
			return 1;
		}

		// Setting underlying connection parameters
		int32 out_size;
		ret_socket->SetReceiveBufferSize(2 * 1024 * 1024, out_size);
		ret_socket->SetReuseAddr(true);

		// Try connection
		const bool b_has_connected = ret_socket->Connect(*connection_addr);

		// If we cannot connect destroy the socket and return
		if (!b_has_connected)
		{
			ret_socket->Close();
			sss->DestroySocket(ret_socket);

			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_CONNECT, TEXT("Connection to Twitch IRC failed!")));
			return 1;
		}

		ConnectionSocket = ret_socket;

		const bool pass_ok = SendIRCMessage(TEXT("PASS ") + Oauth);
		const bool nick_ok = SendIRCMessage(TEXT("NICK ") + Username);
		const bool b_success = pass_ok && nick_ok;
		if(b_success)
		{
			WaitingForAuth = true;
		}
		else
		{
			ret_socket->Close();
			sss->DestroySocket(ret_socket);

			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_CONNECT,
				TEXT("Could not send initial PASS and NICK messages for Auth")));
			return 1;
		}
	}

	while(WaitingForAuth)
	{
		FString connectionMessage = ReceiveFromConnection();
		if(!connectionMessage.IsEmpty())
		{
			if(!(connectionMessage.StartsWith(TEXT(":tmi.twitch.tv 001")) && connectionMessage.Contains(TEXT(":Welcome, GLHF!"))))
			{
				ISocketSubsystem* sss = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
				ConnectionSocket->Close();
				sss->DestroySocket(ConnectionSocket);
				ConnectionSocket = nullptr;
			
				ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_AUTHENTICATE, connectionMessage));
				return 1;
			}

			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::CONNECTED, connectionMessage));
			
			WaitingForAuth = false;

			if(!Channel.IsEmpty())
			{
				const bool join_ok = SendIRCMessage(TEXT("JOIN #") + Channel);
				if (!join_ok)
				{
					ISocketSubsystem* sss = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
					ConnectionSocket->Close();
					sss->DestroySocket(ConnectionSocket);
					ConnectionSocket = nullptr;
			
					ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::FAILED_TO_AUTHENTICATE, TEXT("Failed to join channel")));
					return 1;
				}
			}
		}
		else
		{
			// Wait a bit
			FPlatformProcess::Sleep(0.1f);
		}
	}

	while(ConnectionSocket != nullptr && !ShouldExit)
	{
		if(ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
		{
			FString connectionMessage = ReceiveFromConnection();
			if (!connectionMessage.IsEmpty())
			{
				FTwitchReceiveMessages newMessages;
				ParseMessage(connectionMessage, newMessages.Usernames, newMessages.Messages);
				if(newMessages.Messages.Num())
				{
					ReceivingQueue->Enqueue(newMessages);
				}
			}

			// Send our messages
			FTwitchSendMessage sendMessage;
			while(SendingQueue->Dequeue(sendMessage))
			{
				if(sendMessage.Type == ETwitchSendMessageType::CHAT_MESSAGE)
				{
					if(!sendMessage.Channel.IsEmpty())
					{
						// Specific user private message
						SendIRCMessage(sendMessage.Message, sendMessage.Channel);
					}
					else if(!Channel.IsEmpty())
					{
						// To the currently joined channel
						SendIRCMessage(sendMessage.Message, Channel);
					}
					else
					{
						ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::ERROR,
							TEXT("Cannot send message. No channel specified, and not joined to a channel.")));
					}
				}
				else if(sendMessage.Type == ETwitchSendMessageType::JOIN_MESSAGE)
				{
					if(!Channel.IsEmpty())
					{
						SendIRCMessage(TEXT("PART #") + Channel);
					}
					Channel = sendMessage.Channel;
					if(!Channel.IsEmpty())
					{
						SendIRCMessage(TEXT("JOIN #") + Channel);
					}
				}
			}
			
			// Sleep a bit before pulling more messages
			FPlatformProcess::Sleep(0.1f);
		}
		else
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::DISCONNECTED, TEXT("Lost connection to server")));
			ShouldExit = true;
		}
	}

	if(ConnectionSocket)
	{
		if(ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
		{
			if(!Channel.IsEmpty())
			{
				// Part ways
				SendIRCMessage(TEXT("PART #") + Channel);
			}
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::DISCONNECTED, TEXT("Diconnected by request gracefully")));
		}
		
		ConnectionSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ConnectionSocket);
		ConnectionSocket = nullptr;
	}
	
	return 0;
}

bool FTwitchMessageReceiver::SendIRCMessage(const FString& message, const FString channel)
{
	// Only operate on existing and connected sockets
	if (ConnectionSocket != nullptr && ConnectionSocket->GetConnectionState() == ESocketConnectionState::SCS_Connected)
	{
		FString messageOut = message;
		// If the user specified a receiver format the message appropriately ("PRIVMSG")
		if (!channel.IsEmpty())
		{
			messageOut = FString::Printf(TEXT("PRIVMSG #%s :%s"), *channel, *message);
		}
		messageOut += TEXT("\n"); // Using C style strlen needs a terminator character or it will crash
		const TCHAR* serialized_message = GetData(messageOut);
		const int32 size = FCString::Strlen(serialized_message);
		int32 out_sent;
		return ConnectionSocket->Send(reinterpret_cast<const uint8*>(TCHAR_TO_UTF8(serialized_message)), size, out_sent);
	}
	
	return false;
}

void FTwitchMessageReceiver::Stop()
{
	ShouldExit = true;
}

void FTwitchMessageReceiver::Exit()
{
}

void FTwitchMessageReceiver::PullMessages(TArray<FString>& usernamesOut, TArray<FString>& messagesOut)
{
	if(ReceivingQueue.IsValid() && !ReceivingQueue->IsEmpty())
	{
		FTwitchReceiveMessages message;
		while(ReceivingQueue->Dequeue(message))
		{
			usernamesOut.Append(message.Usernames);
			messagesOut.Append(message.Messages);
		}
	}
}

void FTwitchMessageReceiver::SendMessage(const ETwitchSendMessageType type, const FString& message, const FString& channel)
{
	if(SendingQueue.IsValid())
	{
		SendingQueue->Enqueue(FTwitchSendMessage {type, message, channel});
	}
}

bool FTwitchMessageReceiver::PullConnectionMessage(ETwitchConnectionMessage& statusOut, FString& messageOut)
{
	TwitchConnectionPair connectionPair;
	if(ConnectionQueue->Dequeue(connectionPair))
	{
		statusOut = connectionPair.Key;
		messageOut = connectionPair.Value;
		return true;
	}

	return false;
}

void FTwitchMessageReceiver::StopConnection(bool waitTillComplete)
{
	if(MessagesThread)
	{
		ShouldExit = true;
		if(waitTillComplete)
		{
			MessagesThread->Kill(true);
		}
	}
}

FString FTwitchMessageReceiver::ReceiveFromConnection() const
{
	TArray<uint8> data;
	uint32 data_size;
	bool received = false;
	if (ConnectionSocket->HasPendingData(data_size))
	{
		received = true;
		data.SetNumUninitialized(data_size); // Make space for the data
		int32 data_read;
		ConnectionSocket->Recv(data.GetData(), data.Num(), data_read); // Receive the data. Hopefully the buffer is large enough
	}

	FString connectionMessage;
	if (received)
	{
		connectionMessage = ANSIBytesToString(data.GetData(), data.Num());
	}

	return connectionMessage;
}

void FTwitchMessageReceiver::ParseMessage(const FString& message, TArray<FString>& out_sender_username, TArray<FString>& messagesOut)
{
	messagesOut.Reset();
	
	TArray<FString> message_lines;
	message.ParseIntoArrayLines(message_lines); // A single "message" from Twitch IRC could include multiple lines. Split them now

	// Parse each line into its parts
	// Each line from Twitch contains meta information and content
	// Also need to check if the message is a PING sent from Twitch to check if the connection is alive
	// This is in the form "PING :tmi.twitch.tv" to which we need to reply with "PONG :tmi.twitch.tv"
	for (int32 cycle_line = 0; cycle_line < message_lines.Num(); cycle_line++)
	{
		// If we receive a PING immediately reply with a PONG and skip the line parsing
		if (message_lines[cycle_line] == TEXT("PING :tmi.twitch.tv"))
		{
			SendIRCMessage(TEXT("PONG :tmi.twitch.tv"));
			continue; // Skip line parsing
		}

		// Parsing line
		// Basic message form is ":twitch_username!twitch_username@twitch_username.tmi.twitch.tv PRIVMSG #channel :message here"
		// So we can split the message into two parts based off the ":" character: meta[0] and content[1..n]
		// Also have to account for	possible ":" inside the content itself
		TArray<FString> message_parts;
		message_lines[cycle_line].ParseIntoArray(message_parts, TEXT(":"));
		if(!message_parts.Num())
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::MESSAGE, message_lines[cycle_line]));
			continue;
		}

		// Meta parsing
		// Meta info is split by whitespaces
		TArray<FString> meta;
		message_parts[0].ParseIntoArrayWS(meta);
		if(meta.Num() < 2)
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::MESSAGE, message_lines[cycle_line]));
			continue;
		}

		// Assume at this point the message is from a user, but just in case set it beforehand
		// This is so that we can return an "empty" user if the message was of any other kind
		// For example, messages from the server (like upon connection) don't have a username
		FString sender_username;
		if (meta[1] == TEXT("PRIVMSG")) // Type of message should always be in position 1 (or at least I hope so)
		{
			// Username should be the first part before the first "!"
			meta[0].Split(TEXT("!"), &sender_username, nullptr);
		}

		if (sender_username.IsEmpty())
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::MESSAGE, message_lines[cycle_line]));
			continue; // Skip line
		}

		// Some messages correspond to events sent by the server (JOIN etc.)
		// In that case the message part is only one
		if (message_parts.Num() > 1)
		{
			// Content of the message is composed by all parts of the message from message_parts[1] on
			FString message_content = message_parts[1];
			if (message_parts.Num() > 2)
			{
				for (int32 cycle_content = 2; cycle_content < message_parts.Num(); cycle_content++)
				{
					// Add back the ":" that was used as the splitter
					message_content += TEXT(":") + message_parts[cycle_content];
				}
			}
			messagesOut.Add(message_content);
			out_sender_username.Add(sender_username);
		}
		else if(message_parts.Num())
		{
			ConnectionQueue->Enqueue(TwitchConnectionPair(ETwitchConnectionMessage::MESSAGE, message_parts[0]));
		}
	}
}

// Sets default values for this component's properties
UTwitchIRCComponent::UTwitchIRCComponent()
	: TwitchMessageReceiver(nullptr)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UTwitchIRCComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if(TwitchMessageReceiver.IsValid())
	{
		bool stillConnected = true;
		ETwitchConnectionMessage status; FString message;
		while(TwitchMessageReceiver->PullConnectionMessage(status, message))
		{
			OnConnectionMessage.Broadcast(status, message);
			if(status == ETwitchConnectionMessage::FAILED_TO_CONNECT ||
				status == ETwitchConnectionMessage::FAILED_TO_AUTHENTICATE ||
				status == ETwitchConnectionMessage::DISCONNECTED)
			{
				stillConnected = false;
			}
		}

		if(!stillConnected)
		{
			PrimaryComponentTick.SetTickFunctionEnable(false);
			TwitchMessageReceiver = nullptr;
		}
		else
		{
			TArray<FString> usernames; TArray<FString> messages;
			TwitchMessageReceiver->PullMessages(usernames, messages);
			check(usernames.Num() == messages.Num());
			for(int32 messageIndex = 0; messageIndex < messages.Num(); ++messageIndex)
			{
				OnMessageReceived.Broadcast(messages[messageIndex], usernames[messageIndex]);
			}
		}
	}
	else
	{
		PrimaryComponentTick.SetTickFunctionEnable(false);
	}
}

void UTwitchIRCComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	if(TwitchMessageReceiver.IsValid())
	{
		TwitchMessageReceiver->StopConnection(true);
	}
}

void UTwitchIRCComponent::Connect(const FString& oauth, const FString& username, const FString& channel)
{
	if(TwitchMessageReceiver.IsValid())
	{
		OnConnectionMessage.Broadcast(ETwitchConnectionMessage::ERROR, TEXT("Already connected / connecting / pending!"));
		return;
	}
	if(oauth.IsEmpty() || username.IsEmpty())
	{
		OnConnectionMessage.Broadcast(ETwitchConnectionMessage::ERROR, TEXT("Invalid connection parameters. Check your strings."));
		return;
	}

	// Create the connection and messaging thread
	TwitchMessageReceiver = MakeUnique<FTwitchMessageReceiver>();
	TwitchMessageReceiver->StartConnection(oauth, username, channel);
	// Tick our component which pulls messages off the queue
	PrimaryComponentTick.SetTickFunctionEnable(true);
}

bool UTwitchIRCComponent::SendChatMessage(const FString& message, const FString channel)
{
	if(TwitchMessageReceiver.IsValid())
	{
		TwitchMessageReceiver->SendMessage(ETwitchSendMessageType::CHAT_MESSAGE, message, channel);
		return true;
	}

	return false;
}

void UTwitchIRCComponent::JoinChannel(const FString& channel)
{
	if(!TwitchMessageReceiver.IsValid())
	{
		return;
	}

	TwitchMessageReceiver->SendMessage(ETwitchSendMessageType::JOIN_MESSAGE, TEXT(""), channel);
}

void UTwitchIRCComponent::Disconnect()
{
	if(!TwitchMessageReceiver.IsValid())
	{
		return;
	}
	
	TwitchMessageReceiver->StopConnection(false);
}
