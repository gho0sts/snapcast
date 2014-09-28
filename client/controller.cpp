#include "controller.h"
#include <iostream>
#include <string>
#include <memory>
#include <unistd.h>
#include "oggDecoder.h"
#include "pcmDecoder.h"
#include "alsaPlayer.h"
#include "timeProvider.h"
#include "message/serverSettings.h"
#include "message/timeMsg.h"
#include "message/requestMsg.h"
#include "message/ackMsg.h"
#include "message/commandMsg.h"

using namespace std;


Controller::Controller() : MessageReceiver(), active_(false), sampleFormat(NULL), decoder(NULL)
{
}


void Controller::onException(ClientConnection* connection, const std::exception& exception)
{
	cout << "onException: " << exception.what() << "\n";
}


void Controller::onMessageReceived(ClientConnection* connection, const BaseMessage& baseMessage, char* buffer)
{
	if (baseMessage.type == message_type::payload)
	{
		if ((stream != NULL) && (decoder != NULL))
		{
			PcmChunk* pcmChunk = new PcmChunk(*sampleFormat, 0);
			pcmChunk->deserialize(baseMessage, buffer);
//cout << "chunk: " << pcmChunk->payloadSize;
			if (decoder->decode(pcmChunk))
			{
				stream->addChunk(pcmChunk);
//cout << ", decoded: " << pcmChunk->payloadSize << ", Duration: " << pcmChunk->getDuration() << ", sec: " << pcmChunk->timestamp.sec << ", usec: " << pcmChunk->timestamp.usec/1000 << ", type: " << pcmChunk->type << "\n";
			}
			else
				delete pcmChunk;
		}
	}
}


void Controller::start(const PcmDevice& pcmDevice, const std::string& _ip, size_t _port)
{
	ip = _ip;
	pcmDevice_ = pcmDevice;
	clientConnection = new ClientConnection(this, ip, _port);
	controllerThread = new thread(&Controller::worker, this);
}


void Controller::stop()
{
	active_ = false;
}


void Controller::worker()
{
//	Decoder* decoder;
	active_ = true;
	decoder = NULL;

	while (active_)
	{
		try
		{
			clientConnection->start();
			RequestMsg requestMsg(serversettings);
			shared_ptr<ServerSettings> serverSettings(NULL);
			while (!(serverSettings = clientConnection->sendReq<ServerSettings>(&requestMsg, 1000)));
			cout << "ServerSettings buffer: " << serverSettings->bufferMs << "\n";

			requestMsg.request = sampleformat;
			while (!(sampleFormat = clientConnection->sendReq<SampleFormat>(&requestMsg, 1000)));
			cout << "SampleFormat rate: " << sampleFormat->rate << ", bits: " << sampleFormat->bits << ", channels: " << sampleFormat->channels << "\n";

			requestMsg.request = header;
			shared_ptr<HeaderMessage> headerChunk(NULL);
			while (!(headerChunk = clientConnection->sendReq<HeaderMessage>(&requestMsg, 1000)));
			cout << "Codec: " << headerChunk->codec << "\n";
			if (headerChunk->codec == "ogg")
				decoder = new OggDecoder();
			else if (headerChunk->codec == "pcm")
				decoder = new PcmDecoder();
			decoder->setHeader(headerChunk.get());

			RequestMsg timeReq(timemsg);
			for (size_t n=0; n<50; ++n)
			{
				shared_ptr<TimeMsg> reply = clientConnection->sendReq<TimeMsg>(&timeReq, 2000);
				if (reply)
				{
					double latency = (reply->received.sec - reply->sent.sec) + (reply->received.usec - reply->sent.usec) / 1000000.;
					TimeProvider::getInstance().setDiffToServer((reply->latency - latency) * 1000 / 2);
					usleep(1000);
				}
			}
			cout << "diff to server [ms]: " << TimeProvider::getInstance().getDiffToServer<chronos::msec>().count() << "\n";

			stream = new Stream(*sampleFormat);
			stream->setBufferLen(serverSettings->bufferMs);

			Player player(pcmDevice_, stream);
			player.start();

			CommandMsg startStream("startStream");
			shared_ptr<AckMsg> ackMsg(NULL);
			while (!(ackMsg = clientConnection->sendReq<AckMsg>(&startStream, 1000)));

			try
			{
				while (active_)
				{
					usleep(500*1000);
                    shared_ptr<TimeMsg> reply = clientConnection->sendReq<TimeMsg>(&timeReq, 1000);
                    if (reply)
                    {
                        double latency = (reply->received.sec - reply->sent.sec) + (reply->received.usec - reply->sent.usec) / 1000000.;
                        TimeProvider::getInstance().setDiffToServer((reply->latency - latency) * 1000 / 2);
//                        cout << "Median: " << TimeProvider::getInstance().getDiffToServer() << "\n";
                    }
				}
			}
			catch (const std::exception& e)
			{
				cout << "Exception in Controller::worker(): " << e.what() << "\n";
				cout << "Stopping player\n";
				player.stop();
				cout << "Deleting stream\n";
				delete stream;
				stream = NULL;
				cout << "done\n";
				throw;
			}
		}
		catch (const std::exception& e)
		{
			cout << "Exception in Controller::worker(): " << e.what() << "\n";
			if (decoder != NULL)
				delete decoder;
			decoder = NULL;
			cout << "Stopping clientConnection\n";
			clientConnection->stop();
			cout << "done\n";
			usleep(1000000);
		}
	}
}



