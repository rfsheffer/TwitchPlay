// Copyright (C) Simone Di Gravio <email: altairjp@gmail.com> - All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "CoreTypes.h"
#include "Components/ActorComponent.h"
#include "Networking.h"
#include "TwitchIRCComponent.generated.h"

UENUM(BlueprintType)
enum class ETwitchConnectionMessage : uint8
{
	// A connection and authentication was established.
	CONNECTED,
	// Failed to connect.
	FAILED_TO_CONNECT,
	// Failed to authenticate.
	FAILED_TO_AUTHENTICATE,
	// A general error, doesn't mean the connection was terminated.
	ERROR,
	// General message from the server
	MESSAGE,
	// Disconnected from server.
	DISCONNECTED
};

/**
 * Declaration of delegate type for messages received from chat.
 * Delegate signature should receive two parameters:
 * _message (const FString&) - Message received.
 * _username (const FString&) - Username of who sent the message.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTwitchMessageReceived, const FString&, message, const FString&, username);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTwitchConnectionMessage, const ETwitchConnectionMessage, type, const FString&, message);

// Blob of user messages received
struct FTwitchReceiveMessages
{
	TArray<FString> Usernames;
	TArray<FString> Messages;
};

enum class ETwitchSendMessageType : uint8
{
	// User Chat Message
	CHAT_MESSAGE,

	// Join new channel message
	JOIN_MESSAGE,
};

struct FTwitchSendMessage
{
	// The message type
	ETwitchSendMessageType Type;
	// The message
	FString Message;
	// The channel (can be empty)
	FString Channel;
};

/**
 * Twitch messages reciever runnable
 */
class FTwitchMessageReceiver final : public FRunnable
{
public:
	using TwitchConnectionPair = TPair<ETwitchConnectionMessage, FString>;
	using FTwitchReceiveMessagesQueue = TQueue<FTwitchReceiveMessages, EQueueMode::Spsc>;
	using FTwitchSendMessagesQueue = TQueue<FTwitchSendMessage, EQueueMode::Spsc>;
	using FTwitchConnectionQueue = TQueue<TwitchConnectionPair, EQueueMode::Spsc>;
	
	FTwitchMessageReceiver();
	virtual ~FTwitchMessageReceiver();

	void StartConnection(const FString& _oauth, const FString& _username, const FString& _channel);

	//
	// FRunnable interface.
	//
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	void PullMessages(TArray<FString>& usernamesOut, TArray<FString>& messagesOut);
	void SendMessage(const ETwitchSendMessageType type, const FString& message, const FString& channel);
	bool PullConnectionMessage(ETwitchConnectionMessage& statusOut, FString& messageOut);

	void StopConnection(bool waitTillComplete);

private:

	FString ReceiveFromConnection() const;

	/**
	* Parses the message received from Twitch IRC chat in order to only get the content of the message.
	* Since a single "message" could actually include multiple lines an array of strings is returned.
	*
	* @param message - Message to parse
	* @param out_sender_username - The username(s) of the message sender(s). In sync with the return array.
	* @param messagesOut - Parsed messages.
	*
	*/
	void ParseMessage(const FString& message, TArray<FString>& out_sender_username, TArray<FString>& messagesOut);

	/**
	 * Send a message on the connected socket
	 * @param message - The message to send
	 * @param channel - The channel (or user) to send this message to
	 */
	bool SendIRCMessage(const FString& message, const FString channel = TEXT(""));

	// Sending and recieving queues
	TUniquePtr<FTwitchSendMessagesQueue> SendingQueue;
	TUniquePtr<FTwitchReceiveMessagesQueue> ReceivingQueue;

	// Connection status queue
	TUniquePtr<FTwitchConnectionQueue> ConnectionQueue;

	FSocket* ConnectionSocket;

	FRunnableThread* MessagesThread;

	FThreadSafeBool ShouldExit;

	// Authentication token. Need to get it from official Twitch API
	FString Oauth;

	// Username. Must be in lowercaps
	FString Username;

	// Channel to join upon successful connection	
	FString Channel;

	// True while we are waiting for the auth reply from the server
	bool WaitingForAuth;
};

/**
 * Makes communication with Twitch IRC possible through UE4 sockets.
 * You can send and receive messages to/from channel chat.
 * Subscribe to OnMessageReceived to know when a message has harrived.
 * Remember to first Connect(), SetUserInfo() and then AuthenticateTwitchIRC() before trying to send messages.
 */
UCLASS(ClassGroup = (TwitchAPI), meta = (BlueprintSpawnableComponent))
class TWITCHPLAY_API UTwitchIRCComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Event called each time a message is received
	UPROPERTY(BlueprintAssignable, Category = "Message Events")
	FTwitchMessageReceived OnMessageReceived;

	// Event called each time a connection message occurs.
	// Use this to determine if the connection was successful, or was disconnected, or an error occured.
	// Also includes general server messages from connection commands, join commands, etc.
	UPROPERTY(BlueprintAssignable, Category = "Message Events")
	FTwitchConnectionMessage OnConnectionMessage;

private:

	// Message receiver runnable
	TUniquePtr<FTwitchMessageReceiver> TwitchMessageReceiver;

public:

	// Sets default values for this component's properties
	UTwitchIRCComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	* Creates a socket and tries to connect to Twitch IRC server.
	*
	* @param oauth - Oauth token to use. Get one from official Twitch APIs.
	* @param username - Username to login with. All low caps.
	* @param channel - The channel to join upon connection. (optional, can call JoinChannel later)
	*/
	UFUNCTION(BlueprintCallable, Category = "Setup")
    void Connect(const FString& oauth, const FString& username, const FString& channel);
	
	/**
	 * Send a message on the connected socket
	 * @param message - The message
	 * @param channel - The channel (or user) to send this message to
	 * @return Whether the message was sent to the worker thread. Check your connection callback for errors.
	 */
	UFUNCTION(BlueprintCallable, Category = "Messages")
	bool SendChatMessage(const FString& message, const FString channel = TEXT(""));

	/**
	 * If connected, join a new channel. If already in a channel, will leave it before joining the new one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Setup")
	void JoinChannel(const FString& channel);

	/**
	 * If connected, disconnects
	 */
	UFUNCTION(BlueprintCallable, Category = "Setup")
	void Disconnect();
};
