// Copyright (C) Simone Di Gravio <email: altairjp@gmail.com> - All Rights Reserved

#define DEBUG_MSG(msg) GEngine->AddOnScreenDebugMessage( -1 , 6 , FColor::Red , msg ) 

#include "Components/TwitchPlayComponent.h"

UTwitchPlayComponent::UTwitchPlayComponent()
{
	bound_events_ = TMap<FString, FOnCommandReceived>();
	OnMessageReceived.AddDynamic(this, &UTwitchPlayComponent::MessageReceivedHandler);
}

void UTwitchPlayComponent::SetupEncapsulationChars(const FString& _command_char, const FString& _options_char)
{
	command_encapsulation_char_ = _command_char;
	options_encapsulation_char_ = _options_char;
}

bool UTwitchPlayComponent::RegisterCommand(const FString& _command_name, const FOnCommandReceived& _callback_function, FString& _out_result)
{
	// No reason to register an empty command
	if (_command_name.IsEmpty())
	{
		_out_result = TEXT("Command type string is invalid");
		return false;
	}

	// Pointer to the command in the event map, if present
	// If the command is found I can use this to switch from the previous function and bind the new one
	FOnCommandReceived* registered_command = bound_events_.Find(_command_name);

	// If the command we want to register is already in the event map 
	// copy the new delegate object info into it   
	// For optimization purposes don't delete the entry in order to create a new one.
	if (registered_command != nullptr)
	{
		*registered_command = _callback_function;
		_out_result = _command_name + TEXT(" command registered. It overwrote a previous registration of the same type");
	}
	else
	{
		// If the command is not registered yet create a new entry for it
		// and copy the incoming delegate object info to the new delegate object
		bound_events_.Add(_command_name, _callback_function);
		_out_result = _command_name + TEXT(" command registered");
	}
	return true;
}

bool UTwitchPlayComponent::UnregisterCommand(const FString& _command_name, FString& _out_result)
{
	// No reason to unregister an empty command 
	if (_command_name == "")
	{
		_out_result = TEXT("Command type string is invalid");
		return false;
	}

	if (bound_events_.Remove(_command_name) == 0)
	{
		_out_result = TEXT("No command of this type was registered");
		return false;
	}
	
	_out_result = _command_name + TEXT(" unregistered");
	return true;
}

void UTwitchPlayComponent::MessageReceivedHandler(const FString & _message, const FString & _username)
{
	const FString command = GetCommandString(_message);

	// No reason to search for the command in the event map, there isn't any
	if (command.IsEmpty())
	{
		return;
	}

	FOnCommandReceived* registered_command = bound_events_.Find(command);

	// If the command was registered proceed with finding any command options
	// Then fire the event
	if (registered_command != nullptr)
	{
		TArray<FString> command_options;
		GetCommandOptionsStrings(_message, command_options);
		registered_command->ExecuteIfBound(command, command_options, _username);
	}
}

FString GetDelimitedString(const FString & _in_string, const FString & _delimiter)
{
	// No delimited string can be found on an empty string
	if (_in_string.IsEmpty())
	{
		return TEXT("");
	}

	// Where does the delimiter start?
	// Remember that the delimiter can be more than 1 character, so we need to add
	// the delimiter length to find the actual start of the delimited string
	const int32 command_start_index = _in_string.Find(_delimiter);

	// If the message did not contain any start delimiter no command can be found
	// Also, if the start delimiter is at the end of the string no command can be found
	if (command_start_index == INDEX_NONE || command_start_index + _delimiter.Len() == _in_string.Len())
	{
		return TEXT("");
	}

	// Search for the end of the command delimiter
	// The starting position for the search is the index of the previous delimiter plus 
	// the actual length of the delimiter (start search from at least one char ahead)
	const int32 command_end_index = _in_string.Find(_delimiter, ESearchCase::IgnoreCase, ESearchDir::FromStart, command_start_index + _delimiter.Len());

	// If we did not find an end delimiter no encapsulated string can be found
	if (command_end_index == INDEX_NONE)
	{
		return TEXT("");
	}

	// If we have the two delimiter positions get the string inbetween them
	return _in_string.Mid(command_start_index + _delimiter.Len(), (command_end_index - (command_start_index + _delimiter.Len())));
}

FString UTwitchPlayComponent::GetCommandString(const FString & _message) const
{
	// Only the first command is accepted
	FString ret_command = GetDelimitedString(_message, command_encapsulation_char_);
	return ret_command;
}

void UTwitchPlayComponent::GetCommandOptionsStrings(const FString & _message, TArray<FString>& optionsOut) const
{
	const FString options = GetDelimitedString(_message, options_encapsulation_char_);
	options.ParseIntoArray(optionsOut, TEXT(","));
}
