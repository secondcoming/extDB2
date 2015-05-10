/*
Copyright (C) 2012 Prithu "bladez" Parker <https://github.com/bladez-/bercon>
Copyright (C) 2014 Declan Ireland <http://github.com/torndeco/extDB>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

 * Change Log
 * Changed Code to use Poco Net Library & Poco Checksums
*/


#include "rcon.h"

#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef RCON_APP
	#include <boost/program_options.hpp>
	#include <boost/thread/thread.hpp>
	#include <fstream>
#else
	#include "../abstract_ext.h"
#endif

#include <boost/algorithm/string/predicate.hpp>
#include <boost/crc.hpp>

#include <Poco/AbstractCache.h>
#include <Poco/ExpireCache.h>
#include <Poco/SharedPtr.h>
#include <Poco/StringTokenizer.h>

#include <Poco/Net/DatagramSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/NetException.h>

#include <Poco/Stopwatch.h>
#include <Poco/Thread.h>

#include <Poco/Exception.h>


void Rcon::init(std::shared_ptr<spdlog::logger> ext_logger)
{
	rcon_run_flag = new std::atomic<bool>(false);
	rcon_login_flag = new std::atomic<bool>(false);
	logger.swap(ext_logger);
}


#ifndef RCON_APP
	void Rcon::extInit(AbstractExt *extension)
	{
		extension_ptr = extension;
	}
#endif


void Rcon::createKeepAlive()
{
	std::ostringstream cmdStream;
	cmdStream.put(0xFFu);
	cmdStream.put(0x01);
	cmdStream.put(0x00); // Seq Number
	cmdStream.put('\0');

	std::string cmd = cmdStream.str();
	crc32.reset();
	crc32.process_bytes(cmd.data(), cmd.length());
	long int crcVal = crc32.checksum();

	std::stringstream hexStream;
	hexStream << std::setfill('0') << std::setw(sizeof(int)*2);
	hexStream << std::hex << crcVal;
	std::string crcAsHex = hexStream.str();

	unsigned char reversedCrc[4];
	unsigned int x;

	std::stringstream converterStream;
	for (int i = 0; i < 4; i++)
	{
		converterStream << std::hex << crcAsHex.substr(6-(2*i),2).c_str();
		converterStream >> x;
		converterStream.clear();
		reversedCrc[i] = x;
	}

	// Create Packet
	std::stringstream cmdPacketStream;
	cmdPacketStream.put(0x42); // B
	cmdPacketStream.put(0x45); // E
	cmdPacketStream.put(reversedCrc[0]); // 4-byte Checksum
	cmdPacketStream.put(reversedCrc[1]);
	cmdPacketStream.put(reversedCrc[2]);
	cmdPacketStream.put(reversedCrc[3]);
	cmdPacketStream << cmd;
	cmdPacketStream.str();

	keepAlivePacket = cmdPacketStream.str();
}


void Rcon::sendPacket()
{
	std::ostringstream cmdStream;
	cmdStream.put(0xFFu);
	cmdStream.put(rcon_packet.packetCode);
	
	if (rcon_packet.packetCode == 0x01)
	{
		switch(rcon_packet.sequenceNum)
		{
			case 2:
				cmdStream.put(2); // seq number
				break;
			case 1:
				cmdStream.put(1); // seq number
				break;
			default:
				//cmdStream.put(0x00); // seq number
				cmdStream.put(0); // seq number
		}
		
		cmdStream << rcon_packet.cmd;
	}
	else if (rcon_packet.packetCode == 0x02)
	{
		cmdStream.put(rcon_packet.cmd_char_workaround);
	}
	else if (rcon_packet.packetCode == 0x00)
	{
		cmdStream << rcon_packet.cmd;
	}

	std::string cmd = cmdStream.str();
	crc32.reset();
	crc32.process_bytes(cmd.data(), cmd.length());
	long int crcVal = crc32.checksum();

	std::stringstream hexStream;
	hexStream << std::setfill('0') << std::setw(sizeof(int)*2);
	hexStream << std::hex << crcVal;
	std::string crcAsHex = hexStream.str();

	unsigned char reversedCrc[4];
	unsigned int x;

	std::stringstream converterStream;
	for (int i = 0; i < 4; i++)
	{
		converterStream << std::hex << crcAsHex.substr(6-(2*i),2).c_str();
		converterStream >> x;
		converterStream.clear();
		reversedCrc[i] = x;
	}

	// Create Packet
	std::stringstream cmdPacketStream;
	cmdPacketStream.put(0x42); // B
	cmdPacketStream.put(0x45); // E
	cmdPacketStream.put(reversedCrc[0]); // 4-byte Checksum
	cmdPacketStream.put(reversedCrc[1]);
	cmdPacketStream.put(reversedCrc[2]);
	cmdPacketStream.put(reversedCrc[3]);
	cmdPacketStream << cmd;

	std::string packet = cmdPacketStream.str();
	dgs.sendBytes(packet.data(), packet.size());
}


void Rcon::extractData(int pos, std::string &result)
{
	std::stringstream ss;
	for(size_t i = pos; i < buffer_size; ++i)
	{
		ss << buffer[i];
	}
	result = ss.str();
}


void Rcon::updateLogin(std::string address, int port, std::string password)
{
	createKeepAlive();

	rcon_login.address = address;
	rcon_login.port = port;
	delete rcon_login.password;
	char *passwd = new char[password.size() + 1];
	std::strcpy(passwd, password.c_str());
	rcon_login.password = passwd;
}


void Rcon::connect()
{
	*rcon_login_flag = false;
	*rcon_run_flag = true;

	// Login Packet
	rcon_packet.cmd = rcon_login.password;
	rcon_packet.packetCode = 0x00;
	rcon_packet.sequenceNum = 0;
	sendPacket();
	logger->info("Rcon: Sent Login Info");
	
	// Reset Timer
	rcon_timer.start();
}


bool Rcon::status()
{
	return *rcon_login_flag;
}


void Rcon::run()
{
	Poco::Net::SocketAddress sa(rcon_login.address, rcon_login.port);
	dgs.connect(sa);
	connect();
	mainLoop();
}


void Rcon::addCommand(std::string &command)
{
	if (*rcon_run_flag)
	{
		std::lock_guard<std::mutex> lock(mutex_rcon_commands);
		rcon_commands.push_back(std::move(std::make_pair(0, command)));
	}
}


void Rcon::getMissions(std::string &command, unsigned int &unique_id)
{
	if (*rcon_run_flag)
	{
		#ifndef RCON_APP
			{
				std::lock_guard<std::mutex> lock(mutex_missions_requests);
				missions_requests.push_back(unique_id);
			}
		#endif
		{
			std::lock_guard<std::mutex> lock(mutex_rcon_commands);
			rcon_commands.push_back(std::move(std::make_pair(1, command)));
		}
	}
}


void Rcon::getPlayers(std::string &command, unsigned int &unique_id)
{
	if (*rcon_run_flag)
	{
		#ifndef RCON_APP
			{
				std::lock_guard<std::mutex> lock(mutex_players_requests);
				players_requests.push_back(unique_id);
			}
		#endif
		{
			std::lock_guard<std::mutex> lock(mutex_rcon_commands);
			rcon_commands.push_back(std::move(std::make_pair(2, command)));
		}
	}
}


void Rcon::disconnect()
{
	*rcon_run_flag = false;	
}


void Rcon::processMessage(int &sequenceNum, std::string &message)
{
	std::vector<std::string> info_vector;
	switch (sequenceNum)
	{
		case 2: //Player Listings
		{
			logger->info("Server Msg0: {0}", message);
			Poco::StringTokenizer tokens(message, "\n");
			for (int i = 3; i < (tokens.count() - 1); ++i)
			{
				std::string playerinfo = tokens[i];
				playerinfo.erase(std::unique(playerinfo.begin(), playerinfo.end(), [](char a, char b) { return a == ' ' && b == ' '; } ), playerinfo.end() );
				Poco::StringTokenizer player_tokens(playerinfo, " ");
				for (auto token : player_tokens)
				{
					logger->info("Server Msg0-0: {0}", token);
				}
				if (player_tokens.count() >= 5)
				{
					RconPlayerInfo player_data;

					/*
							struct RconPlayerInfo   
							{
								std::string number;
								std::string ip;
								std::string port;
								std::string guid;
								bool verified;
								std::string player_name;
							};
					*/

					player_data.number = player_tokens[0]; // RCon Player Number

					auto found = player_tokens[1].find(":");
					player_data.ip = player_tokens[1].substr(0,found-1)
					player_data.port = player_tokens[1].substr(found+1)

					player_data.ping =player_tokens[2]; // Ping

					if (boost::algorithm::ends_with(player_tokens[3], "(OK)"))
					{
						player_data.verified = true;
						player_data.guid = player_tokens[3].substr(0,(player_tokens.size()-4));
					}
					else
					{
						player_data.verified = false;
						player_data.guid = player_tokens[3].substr(0,(player_tokens.size()-12));
					}
					found = tokens[i].find(")");
					player_data.name = tokens[i].substr(found+2)
				}
				else
				{
					// ERROR
				}



			}
		}
		break;

		case 1: //Mission Listings
		{
			logger->info("Server Msg1: {0}", message);
			Poco::StringTokenizer tokens(message, "\n");
			for (int i = 1; i < (tokens.count()); ++i)
			{
				if (boost::algorithm::ends_with(tokens[i], ".pbo"))
				{
					info_vector.push_back(tokens[i].substr(0, tokens[i].size() - 4));
				}
				else
				{
					info_vector.push_back(tokens[i]);
				}
			}

			#ifndef RCON_APP
				std::string result;
				if (info_vector.empty())
				{
					result  = "[1,[]]";
				}
				else
				{
					result = "[1,[";
					for(auto &info : info_vector)
					{
						result += info;
						result += ",";
						logger->info("Server Mission: {0}", info);
					}
					result.pop_back();
					result += "]]";
				}

				AbstractExt::resultData result_data;
				result_data.message = result;
				{
					std::lock_guard<std::mutex> lock(mutex_missions_requests);
					for(auto unique_id: missions_requests)
					{
						extension_ptr->saveResult_mutexlock(unique_id, result_data);
					}
					missions_requests.clear();
				}
				
			#else
				for(auto &info : info_vector)
				{
					logger->info("Server Mission: {0}", info);
				}
			#endif
		}
		break;

		default:
			logger->info("Server Num: {0}", sequenceNum);
			logger->info("Server Msg: {0}", message);
			logger->info("");
			break;
	}
}


void Rcon::mainLoop()
{
	*rcon_login_flag = false;

	int elapsed_seconds;
	int sequenceNum;

	// 2 Min Cache for UDP Multi-Part Messages
	Poco::ExpireCache<int, RconMultiPartMsg > rcon_msg_cache(120000);
	
	dgs.setReceiveTimeout(Poco::Timespan(5, 0));
	while (true)
	{
		try 
		{
			buffer_size = dgs.receiveBytes(buffer, sizeof(buffer)-1);
			buffer[buffer_size] = '\0';
			rcon_timer.restart();

			if (buffer[7] == 0x00)
			{
				if (buffer[8] == 0x01)
				{
					logger->warn("Rcon: Logged In");
					*rcon_login_flag = true;
				}
				else
				{
					// Login Failed
					
					logger->warn("Rcon: ACK: {0}", sequenceNum);
					*rcon_login_flag = false;
					disconnect();
					break;
				}
			}
			else if ((buffer[7] == 0x01) && *rcon_login_flag)
			{
				// Rcon Server Ack Message Received
				sequenceNum = buffer[8];
				logger->warn("Rcon: ACK: {0}", sequenceNum);
				
				if (!((buffer[9] == 0x00) && (buffer_size > 9)))
				{
					// Server Received Command Msg
					std::string result;
					extractData(9, result);
					processMessage(sequenceNum, result);
				}
				else
				{
					// Rcon Multi-Part Message Recieved
					int numPackets = buffer[10];
					int packetNum = buffer[11];
					
					std::string partial_msg;
					extractData(12, partial_msg);
					
					if (!(rcon_msg_cache.has(sequenceNum)))
					{
						// Doesn't have sequenceNum in Buffer
						RconMultiPartMsg rcon_mp_msg;
						rcon_mp_msg.first = 1;
						rcon_msg_cache.add(sequenceNum, rcon_mp_msg);
						
						Poco::SharedPtr<RconMultiPartMsg> ptrElem = rcon_msg_cache.get(sequenceNum);
						ptrElem->second[packetNum] = partial_msg;
					}
					else
					{
						// Has sequenceNum in Buffer
						Poco::SharedPtr<RconMultiPartMsg> ptrElem = rcon_msg_cache.get(sequenceNum);
						ptrElem->first = ptrElem->first + 1;
						ptrElem->second[packetNum] = partial_msg;
						
						if (ptrElem->first == numPackets)
						{
							// All packets Received, re-construct message
							std::string result;
							for (int i = 0; i < numPackets; ++i)
							{
								result = result + ptrElem->second[i];
							}
							processMessage(sequenceNum, result);
							rcon_msg_cache.remove(sequenceNum);
						}
					}
				}
			}
			else if (buffer[7] == 0x02)
			{
				if (!*rcon_login_flag)
				{
					// Already Logged In 
					*rcon_login_flag = true;
				}
				else
				{
					// Received Chat Messages
					std::string result;
					extractData(9, result);
					logger->info("CHAT: {0}", result);
					
					// Respond to Server Msgs i.e chat messages, to prevent timeout
					rcon_packet.packetCode = 0x02;
					rcon_packet.cmd_char_workaround = buffer[8];
					rcon_packet.sequenceNum = 0;
					sendPacket();
				}
			}

			if (*rcon_login_flag)
			{
				// Checking for Commands to Send
				std::lock_guard<std::mutex> lock(mutex_rcon_commands);
				for (auto &rcon_command : rcon_commands)
				{
					char *cmd = new char[rcon_command.second.size()+1];
					std::strcpy(cmd, rcon_command.second.c_str());
					
					delete []rcon_packet.cmd;
					rcon_packet.cmd = cmd;
					
					rcon_packet.packetCode = 0x01;
					rcon_packet.sequenceNum = rcon_command.first;
					sendPacket();
				}
				// Clear Comands
				rcon_commands.clear();

				if (!*rcon_run_flag)
				{
					break;
				}
			}
			else
			{
				if (!*rcon_run_flag)
				{
					std::lock_guard<std::mutex> lock(mutex_rcon_commands);
					if (rcon_commands.empty())
					{
						break;
					}
				}
			}
		}
		catch (Poco::TimeoutException&)
		{
			if (!*rcon_run_flag)  // Checking Run Flag
			{
				break;
			}
			else
			{
				elapsed_seconds = rcon_timer.elapsedSeconds();
				if (elapsed_seconds >= 45)
				{
					logger->warn("Rcon: TIMED OUT...");
					rcon_timer.restart();
					connect();
				}
				else if (elapsed_seconds >= 30)
				{
					// Keep Alive
					logger->info("Keep Alive Sending");

					rcon_timer.restart();

					dgs.sendBytes(keepAlivePacket.data(), keepAlivePacket.size());

					logger->info("Keep Alive Sent");
				}
				else if (*rcon_login_flag)
				{
					// Checking for Commands to Send
					std::lock_guard<std::mutex> lock(mutex_rcon_commands);

					for (auto &rcon_command : rcon_commands)
					{
						char *cmd = new char[rcon_command.second.size()+1];
						std::strcpy(cmd, rcon_command.second.c_str());
						
						delete []rcon_packet.cmd;
						rcon_packet.cmd = cmd;
						
						rcon_packet.packetCode = 0x01;
						rcon_packet.sequenceNum = rcon_command.first;
						sendPacket();
					}
					// Clear Comands
					rcon_commands.clear();
				}
			}
		}
		catch (Poco::Net::ConnectionRefusedException& e)
		{
			logger->error("Rcon: Error Connect: {0}", e.displayText());
			disconnect();
		}
		catch (Poco::Exception& e)
		{
			logger->error("Rcon: Error Rcon: {0}", e.displayText());
			disconnect();
		}
	}
	logger->warn("Rcon: Stopping...");
}



#ifdef RCON_APP

	int main(int nNumberofArgs, char* pszArgs[])
	{
		auto console = spdlog::stdout_logger_mt("extDB Console logger");

		boost::program_options::options_description desc("Options");
		desc.add_options()
			("help", "Print help messages")
			("ip", boost::program_options::value<std::string>()->required(), "IP Address for Server")
			("port", boost::program_options::value<int>()->required(), "Port for Server")
			("password", boost::program_options::value<std::string>()->required(), "Rcon Password for Server")
			("file", boost::program_options::value<std::string>(), "File to run i.e rcon restart warnings");

		boost::program_options::variables_map options;

		try 
		{
			boost::program_options::store(boost::program_options::parse_command_line(nNumberofArgs, pszArgs, desc), options);
			
			if (options.count("help") )
			{
				console->info("Rcon Command Line, based off bercon by Prithu \"bladez\" Parker");
				console->info("\t\t @ https://github.com/bladez-/bercon");
				console->info("");
				console->info("");
				console->info("Rewritten for extDB + crossplatform by Torndeco");
				console->info("\t\t @ https://github.com/Torndeco/extDB");
				console->info("");
				console->info("File Option is just for parsing rcon commands to be ran, i.e server restart warnings");
				console->info("\t\t For actually restarts use a cron program to run a script");
				console->info("");
				return 0;
			}
			
			boost::program_options::notify(options);
		}
		catch(boost::program_options::error& e)
		{
			console->error("ERROR: {0}", e.what());
			console->error("ERROR: Desc {0}", desc);
			return 1;
		}

		Rcon rcon;
		rcon.init(console);
		rcon.updateLogin(options["ip"].as<std::string>(), options["port"].as<int>(), options["password"].as<std::string>());
		Poco::Thread thread;
		thread.start(rcon);
		
		if (options.count("file"))
		{
			std::ifstream fin(options["file"].as<std::string>());
			//std::ifstream fin("test");
			if (fin.is_open() == false)
			{
				console->warn("ERROR: File is Open");
				return 1;
			}
			else
			{
				console->info("File is OK");
			}
			
			std::string line;
			while (std::getline(fin, line))
			{
				console->info("{0}", line);
				if (line.empty())
				{
					boost::this_thread::sleep( boost::posix_time::milliseconds(1000) );
					console->info("Sleep", line);
				}
				else
				{
					rcon.addCommand(line);
				}
			}
			console->info("OK");
			rcon.disconnect();
			thread.join();
			return 0;
		}
		else
		{
			console->info("**********************************");
			console->info("**********************************");
			console->info("To talk type ");
			console->info("SAY -1 Server Restart in 10 mins");
			console->info();
			console->info("To see all players type");
			console->info("players");
			console->info("**********************************");
			console->info("**********************************");
			console->info();

			std::string input_str;
			unsigned int unique_id = 1;
			for (;;) {
				std::getline(std::cin, input_str);
				if (input_str == "quit")
				{
					console->info("Quitting Please Wait");
					rcon.disconnect();
					thread.join();
					break;
				}
				else if (input_str == "players")
				{
					rcon.getPlayers(input_str, unique_id);	
				}
				else if (input_str == "missions")
				{
					rcon.getMissions(input_str, unique_id);	
				}
				else
				{
					rcon.addCommand(input_str);
				}
			}
			console->info("Quitting");
			return 0;
		}
	}
#endif