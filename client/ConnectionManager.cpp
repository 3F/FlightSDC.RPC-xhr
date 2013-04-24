/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"
#include "ConnectionManager.h"

#include "ResourceManager.h"
#include "DownloadManager.h"
#include "UploadManager.h"
#include "CryptoManager.h"
#include "ClientManager.h"
#include "QueueManager.h"
#include "LogManager.h"
#include "UserConnection.h"

namespace dcpp
{

uint16_t ConnectionManager::iConnToMeCount = 0;

ConnectionManager::ConnectionManager() : floodCounter(0), server(0), secureServer(0), shuttingDown(false)
{
	TimerManager::getInstance()->addListener(this);
	
	features.push_back(UserConnection::FEATURE_MINISLOTS);
	features.push_back(UserConnection::FEATURE_XML_BZLIST);
	features.push_back(UserConnection::FEATURE_ADCGET);
	features.push_back(UserConnection::FEATURE_TTHL);
	features.push_back(UserConnection::FEATURE_TTHF);
	
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BAS0);
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BASE);
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_TIGR);
	adcFeatures.push_back("AD" + UserConnection::FEATURE_ADC_BZIP);
}

void ConnectionManager::listen()
{
	disconnect();
	
	server = new Server(false, static_cast<uint16_t>(SETTING(TCP_PORT)), Socket::getBindAddress());
	
	if (!CryptoManager::getInstance()->TLSOk())
	{
		dcdebug("Skipping secure port: %d\n", SETTING(TLS_PORT));
		return;
	}
	secureServer = new Server(true, static_cast<uint16_t>(SETTING(TLS_PORT)), Socket::getBindAddress());
}

/**
 * Request a connection for downloading.
 * DownloadManager::addConnection will be called as soon as the connection is ready
 * for downloading.
 * @param aUser The user to connect to.
 */
void ConnectionManager::getDownloadConnection(const HintedUser& aUser)
{
	dcassert((bool)aUser.user);
	{
		Lock l(cs);
		ConnectionQueueItem::Iter i = find(downloads.begin(), downloads.end(), aUser.user);
		if (i == downloads.end())
		{
			getCQI(aUser, true);
		}
		else
		{
			DownloadManager::getInstance()->checkIdle(aUser.user);
		}
	}
}

ConnectionQueueItem* ConnectionManager::getCQI(const HintedUser& aUser, bool download)
{
	ConnectionQueueItem* cqi = new ConnectionQueueItem(aUser, download);
	if (download)
	{
		dcassert(find(downloads.begin(), downloads.end(), aUser.user) == downloads.end());
		downloads.push_back(cqi);
	}
	else
	{
		dcassert(find(uploads.begin(), uploads.end(), aUser.user) == uploads.end());
		uploads.push_back(cqi);
	}
	
	fire(ConnectionManagerListener::Added(), cqi);
	return cqi;
}

void ConnectionManager::putCQI(ConnectionQueueItem* cqi)
{
	fire(ConnectionManagerListener::Removed(), cqi);
	if (cqi->getDownload())
	{
		dcassert(find(downloads.begin(), downloads.end(), cqi) != downloads.end());
		downloads.erase(remove(downloads.begin(), downloads.end(), cqi), downloads.end());
	}
	else
	{
		UploadManager::getInstance()->removeDelayUpload(cqi->getUser());
		dcassert(find(uploads.begin(), uploads.end(), cqi) != uploads.end());
		uploads.erase(remove(uploads.begin(), uploads.end(), cqi), uploads.end());
	}
	delete cqi;
}

UserConnection* ConnectionManager::getConnection(bool aNmdc, bool secure) noexcept
{
	UserConnection* uc = new UserConnection(secure);
	uc->addListener(this);
	{
		Lock l(cs);
		userConnections.push_back(uc);
	}
	if (aNmdc)
		uc->setFlag(UserConnection::FLAG_NMDC);
	return uc;
}

void ConnectionManager::putConnection(UserConnection* aConn)
{
	aConn->StoreRatio();
	aConn->removeListener(this);
	aConn->disconnect(true);
	
	Lock l(cs);
	userConnections.erase(remove(userConnections.begin(), userConnections.end(), aConn), userConnections.end());
}

void ConnectionManager::on(TimerManagerListener::Second, uint64_t aTick) noexcept
{
	UserList passiveUsers;
	ConnectionQueueItem::List removed;
	
	{
		Lock l(cs);
		
		uint16_t attempts = 0;
		
		for (ConnectionQueueItem::Iter i = downloads.begin(); i != downloads.end(); ++i)
		{
			ConnectionQueueItem* cqi = *i;
			
			if (cqi->getState() != ConnectionQueueItem::ACTIVE)
			{
				if (!cqi->getUser().user->isOnline())
				{
					// Not online anymore...remove it from the pending...
					removed.push_back(cqi);
					continue;
				}
				
				/*if(   cqi->getUser().user->isSet(User::PASSIVE) &&
				    !ClientManager::getInstance()->isActive(cqi->getUser().hint))
				{
				    passiveUsers.push_back(cqi->getUser());
				    removed.push_back(cqi);
				    continue;
				}*/
				
				if (cqi->getErrors() == -1 && cqi->getLastAttempt() != 0)
				{
					// protocol error, don't reconnect except after a forced attempt
					continue;
				}
				
				if (cqi->getLastAttempt() == 0 || ((SETTING(DOWNCONN_PER_SEC) == 0 || attempts < SETTING(DOWNCONN_PER_SEC)) &&
				                                   cqi->getLastAttempt() + 60 * 1000 * max(1, cqi->getErrors()) < aTick))
				{
					cqi->setLastAttempt(aTick);
					
					QueueItem::Priority prio = QueueManager::getInstance()->hasDownload(cqi->getUser());
					
					if (prio == QueueItem::PAUSED)
					{
						removed.push_back(cqi);
						continue;
					}
					
					bool startDown = DownloadManager::getInstance()->startDownload(prio);
					
					if (cqi->getState() == ConnectionQueueItem::WAITING)
					{
						if (startDown)
						{
							cqi->setState(ConnectionQueueItem::CONNECTING);
							ClientManager::getInstance()->connect(cqi->getUser(), cqi->getToken());
							fire(ConnectionManagerListener::StatusChanged(), cqi);
							attempts++;
						}
						else
						{
							cqi->setState(ConnectionQueueItem::NO_DOWNLOAD_SLOTS);
							fire(ConnectionManagerListener::Failed(), cqi, STRING(ALL_DOWNLOAD_SLOTS_TAKEN));
						}
					}
					else if (cqi->getState() == ConnectionQueueItem::NO_DOWNLOAD_SLOTS && startDown)
					{
						cqi->setState(ConnectionQueueItem::WAITING);
					}
				}
				else if (cqi->getState() == ConnectionQueueItem::CONNECTING && cqi->getLastAttempt() + 50 * 1000 < aTick)
				{
					ClientManager::getInstance()->connectionTimeout(cqi->getUser());
					
					cqi->setErrors(cqi->getErrors() + 1);
					fire(ConnectionManagerListener::Failed(), cqi, STRING(CONNECTION_TIMEOUT));
					cqi->setState(ConnectionQueueItem::WAITING);
				}
			}
		}
		
		for (ConnectionQueueItem::Iter m = removed.begin(); m != removed.end(); ++m)
		{
			putCQI(*m);
		}
		
	}
	
	for (UserList::iterator ui = passiveUsers.begin(); ui != passiveUsers.end(); ++ui)
	{
		QueueManager::getInstance()->removeSource(*ui, QueueItem::Source::FLAG_PASSIVE);
	}
}

void ConnectionManager::on(TimerManagerListener::Minute, uint64_t aTick) noexcept
{
	Lock l(cs);
	
	for (UserConnectionList::const_iterator j = userConnections.begin(); j != userConnections.end(); ++j)
	{
		if (((*j)->getLastActivity() + 180 * 1000) < aTick)
		{
			(*j)->disconnect(true);
		}
	}
}

static const uint32_t FLOOD_TRIGGER = 20000;
static const uint32_t FLOOD_ADD = 2000;

ConnectionManager::Server::Server(bool secure_, uint16_t aPort, const string& ip_ /* = "0.0.0.0" */) : port(0), secure(secure_), die(false)
{
	sock.create();
	sock.setSocketOpt(SO_REUSEADDR, 1);
	ip = ip_;
	port = sock.bind(aPort, ip);
	sock.listen();
	
	start();
}

static const uint32_t POLL_TIMEOUT = 250;

int ConnectionManager::Server::run() noexcept
{
	while (!die)
	{
		try
		{
			while (!die)
			{
				auto ret = sock.wait(POLL_TIMEOUT, Socket::WAIT_READ);
				if (ret == Socket::WAIT_READ)
				{
					ConnectionManager::getInstance()->accept(sock, secure);
				}
			}
		}
		catch (const Exception& e)
		{
			dcdebug("ConnectionManager::Server::run Error: %s\n", e.getError().c_str());
		}
		
		bool failed = false;
		while (!die)
		{
			try
			{
				sock.disconnect();
				sock.create();
				sock.bind(port, ip);
				sock.listen();
				if (failed)
				{
					LogManager::getInstance()->message("Connectivity restored"); // TODO: translate
					failed = false;
				}
				break;
			}
			catch (const SocketException& e)
			{
				dcdebug("ConnectionManager::Server::run Stopped listening: %s\n", e.getError().c_str());
				
				if (!failed)
				{
					LogManager::getInstance()->message("Connectivity error: " + e.getError());
					failed = true;
				}
				
				// Spin for 60 seconds
				for (int i = 0; i < 60 && !die; ++i)
				{
					Thread::sleep(1000);
				}
			}
		}
	}
	return 0;
}

/**
 * Someone's connecting, accept the connection and wait for identification...
 * It's always the other fellow that starts sending if he made the connection.
 */
void ConnectionManager::accept(const Socket& sock, bool secure) noexcept
{
	uint64_t now = GET_TICK();
	
	if (iConnToMeCount > 0)
		iConnToMeCount--;
		
	if (now > floodCounter)
	{
		floodCounter = now + FLOOD_ADD;
	}
	else {
		if (now + FLOOD_TRIGGER < floodCounter)
		{
			Socket s;
			try
			{
				s.accept(sock);
			}
			catch (const SocketException&)
			{
				// ...
			}
			dcdebug("Connection flood detected!\n");
			return;
		}
		else {
			if (iConnToMeCount <= 0)
				floodCounter += FLOOD_ADD;
		}
	}
	UserConnection* uc = getConnection(false, secure);
	uc->setFlag(UserConnection::FLAG_INCOMING);
	uc->setState(UserConnection::STATE_SUPNICK);
	uc->setLastActivity(GET_TICK());
	try {
		uc->accept(sock);
	}
	catch (const Exception&)
	{
		putConnection(uc);
		delete uc;
	}
}

bool ConnectionManager::checkIpFlood(const string& aServer, uint16_t aPort, const string& userInfo)
{
	Lock l(cs);
	
	// Temporary fix to avoid spamming
	if (aPort == 80 || aPort == 2501)
	{
		LogManager::getInstance()->message("Someone (" + userInfo + ") is trying to use your client to spam " + aServer + ":" + Util::toString(aPort) + ", please urge hub owner to fix this");
		return true;
	}
	
	// We don't want to be used as a flooding instrument
	uint8_t count = 0;
	for (UserConnectionList::const_iterator j = userConnections.begin(); j != userConnections.end(); ++j)
	{
	
		const UserConnection& uc = **j;
		
		if (uc.socket == NULL || !uc.socket->hasSocket())
			continue;
			
		if (uc.getRemoteIp() == aServer && uc.getPort() == aPort)
		{
			if (++count >= 5)
			{
				// More than 5 outbound connections to the same addr/port? Can't trust that..
				dcdebug("ConnectionManager::connect Tried to connect more than 5 times to %s:%hu, connect dropped\n", aServer.c_str(), aPort);
				return true;
			}
		}
	}
	return false;
}

void ConnectionManager::nmdcConnect(const string& aServer, uint16_t aPort, const string& aNick, const string& hubUrl, const string& encoding, bool stealth, bool secure)
{
	nmdcConnect(aServer, aPort, 0, BufferedSocket::NAT_NONE, aNick, hubUrl, encoding, stealth, secure);
}

void ConnectionManager::nmdcConnect(const string& aServer, uint16_t aPort, uint16_t localPort, BufferedSocket::NatRoles natRole, const string& aNick, const string& hubUrl, const string& encoding, bool stealth, bool secure)
{
	if (shuttingDown)
		return;
		
	if (checkIpFlood(aServer, aPort, "NMDC Hub: " + hubUrl))
		return;
		
	UserConnection* uc = getConnection(true, secure);
	uc->setToken(aNick);
	uc->setHubUrl(hubUrl);
	uc->setEncoding(encoding);
	uc->setState(UserConnection::STATE_CONNECT);
	uc->setFlag(UserConnection::FLAG_NMDC);
	if (stealth)
	{
		uc->setFlag(UserConnection::FLAG_STEALTH);
	}
	try
	{
		uc->connect(aServer, aPort, localPort, natRole);
	}
	catch (const Exception&)
	{
		putConnection(uc);
		delete uc;
	}
}

void ConnectionManager::adcConnect(const OnlineUser& aUser, uint16_t aPort, const string& aToken, bool secure)
{
	adcConnect(aUser, aPort, 0, BufferedSocket::NAT_NONE, aToken, secure);
}

void ConnectionManager::adcConnect(const OnlineUser& aUser, uint16_t aPort, uint16_t localPort, BufferedSocket::NatRoles natRole, const string& aToken, bool secure)
{
	if (shuttingDown)
		return;
		
	if (checkIpFlood(aUser.getIdentity().getIp(), aPort, "ADC Nick: " + aUser.getIdentity().getNick() + ", Hub: " + aUser.getClientBase().getHubName()))
		return;
		
	UserConnection* uc = getConnection(false, secure);
	uc->setToken(aToken);
	uc->setEncoding(Text::g_utf8);
	uc->setState(UserConnection::STATE_CONNECT);
	uc->setHubUrl(&aUser.getClient() == NULL ? "DHT" : aUser.getClient().getHubUrl());
	if (aUser.getIdentity().isOp())
	{
		uc->setFlag(UserConnection::FLAG_OP);
	}
	try
	{
		uc->connect(aUser.getIdentity().getIp(), aPort, localPort, natRole);
	}
	catch (const Exception&)
	{
		putConnection(uc);
		delete uc;
	}
}

void ConnectionManager::disconnect() noexcept
{
	delete server;
	delete secureServer;
	
	server = secureServer = 0;
}

void ConnectionManager::on(AdcCommand::SUP, UserConnection* aSource, const AdcCommand& cmd) noexcept
{
	if (aSource->getState() != UserConnection::STATE_SUPNICK)
	{
		// Already got this once, ignore...@todo fix support updates
		dcdebug("CM::onSUP %p sent sup twice\n", (void*)aSource);
		return;
	}
	
	bool baseOk = false;
	bool tigrOk = false;
	
	for (StringIterC i = cmd.getParameters().begin(); i != cmd.getParameters().end(); ++i)
	{
		if (i->compare(0, 2, "AD") == 0)
		{
			string feat = i->substr(2);
			if (feat == UserConnection::FEATURE_ADC_BASE || feat == UserConnection::FEATURE_ADC_BAS0)
			{
				baseOk = true;
				// For bas0 tiger is implicit
				if (feat == UserConnection::FEATURE_ADC_BAS0)
				{
					tigrOk = true;
				}
				// ADC clients must support all these...
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_ADCGET);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_MINISLOTS);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_TTHF);
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_TTHL);
				// For compatibility with older clients...
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
			}
			else if (feat == UserConnection::FEATURE_ZLIB_GET)
			{
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_ZLIB_GET);
			}
			else if (feat == UserConnection::FEATURE_ADC_BZIP)
			{
				aSource->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
			}
			else if (feat == UserConnection::FEATURE_ADC_TIGR)
			{
				tigrOk = true;
			}
		}
	}
	
	if (!baseOk)
	{
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "Invalid SUP"));
		aSource->disconnect();
		return;
	}
	
	if (aSource->isSet(UserConnection::FLAG_INCOMING))
	{
		StringList defFeatures = adcFeatures;
		if (BOOLSETTING(COMPRESS_TRANSFERS))
		{
			defFeatures.push_back("AD" + UserConnection::FEATURE_ZLIB_GET);
		}
		aSource->sup(defFeatures);
		aSource->inf(false);
	}
	else {
		aSource->inf(true);
	}
	aSource->setState(UserConnection::STATE_INF);
}

void ConnectionManager::on(AdcCommand::STA, UserConnection*, const AdcCommand& /*cmd*/) noexcept
{

}

void ConnectionManager::on(UserConnectionListener::Connected, UserConnection* aSource) noexcept
{
	if (aSource->isSecure() && !aSource->isTrusted() && !BOOLSETTING(ALLOW_UNTRUSTED_CLIENTS))
	{
		putConnection(aSource);
		QueueManager::getInstance()->removeSource(aSource->getUser(), QueueItem::Source::FLAG_UNTRUSTED);
		return;
	}
	
	dcassert(aSource->getState() == UserConnection::STATE_CONNECT);
	if (aSource->isSet(UserConnection::FLAG_NMDC))
	{
		aSource->myNick(aSource->getToken()); //TODO crash-strong-full-r6730-x64.dmp
		aSource->lock(CryptoManager::getInstance()->getLock(), CryptoManager::getInstance()->getPk() + "Ref=" + aSource->getHubUrl());
	}
	else {
		StringList defFeatures = adcFeatures;
		if (BOOLSETTING(COMPRESS_TRANSFERS))
		{
			defFeatures.push_back("AD" + UserConnection::FEATURE_ZLIB_GET);
		}
		aSource->sup(defFeatures);
		aSource->send(AdcCommand(AdcCommand::SEV_SUCCESS, AdcCommand::SUCCESS, Util::emptyString).addParam("RF", aSource->getHubUrl()));
	}
	aSource->setState(UserConnection::STATE_SUPNICK);
}

void ConnectionManager::on(UserConnectionListener::MyNick, UserConnection* aSource, const string& aNick) noexcept
{
	if (aSource->getState() != UserConnection::STATE_SUPNICK)
	{
		// Already got this once, ignore...
		dcdebug("CM::onMyNick %p sent nick twice\n", (void*)aSource);
		return;
	}
	
	dcassert(aNick.size() > 0);
	dcdebug("ConnectionManager::onMyNick %p, %s\n", (void*)aSource, aNick.c_str());
	dcassert(!aSource->getUser());
	
	if (aSource->isSet(UserConnection::FLAG_INCOMING))
	{
		// Try to guess where this came from...
		pair<string, string> i = expectedConnections.remove(aNick);
		if (i.second.empty())
		{
			dcassert(i.first.empty());
			dcdebug("Unknown incoming connection from %s\n", aNick.c_str());
			putConnection(aSource);
			return;
		}
		aSource->setToken(i.first);
		aSource->setHubUrl(i.second);
		aSource->setEncoding(ClientManager::getInstance()->findHubEncoding(i.second));
	}
	
	const string nick = Text::toUtf8(aNick, aSource->getEncoding());
	const CID cid = ClientManager::getInstance()->makeCid(nick, aSource->getHubUrl());
	
	// First, we try looking in the pending downloads...hopefully it's one of them...
	{
		Lock l(cs);
		for (ConnectionQueueItem::Iter i = downloads.begin(); i != downloads.end(); ++i)
		{
			ConnectionQueueItem* cqi = *i;
			cqi->setErrors(0);
			if ((cqi->getState() == ConnectionQueueItem::CONNECTING || cqi->getState() == ConnectionQueueItem::WAITING) &&
			cqi->getUser().user->getCID() == cid)
			{
				aSource->setUser(cqi->getUser());
				// Indicate that we're interested in this file...
				aSource->setFlag(UserConnection::FLAG_DOWNLOAD);
				break;
			}
		}
	}
	
	if (!aSource->getUser())
	{
		// Make sure we know who it is, i e that he/she is connected...
		
		aSource->setUser(ClientManager::getInstance()->findUser(cid));
		if (!aSource->getUser() || !aSource->getUser()->isOnline())
		{
			dcdebug("CM::onMyNick Incoming connection from unknown user %s\n", nick.c_str());
			putConnection(aSource);
			return;
		}
		// We don't need this connection for downloading...make it an upload connection instead...
		aSource->setFlag(UserConnection::FLAG_UPLOAD);
	}
	
	if (ClientManager::getInstance()->isStealth(aSource->getHubUrl()))
		aSource->setFlag(UserConnection::FLAG_STEALTH);
		
	ClientManager::getInstance()->setIPUser(aSource->getUser(), aSource->getRemoteIp());
	
	if (ClientManager::getInstance()->isOp(aSource->getUser(), aSource->getHubUrl()))
		aSource->setFlag(UserConnection::FLAG_OP);
		
	if (aSource->isSet(UserConnection::FLAG_INCOMING))
	{
		aSource->myNick(aSource->getToken());
		aSource->lock(CryptoManager::getInstance()->getLock(), CryptoManager::getInstance()->getPk());
	}
	
	aSource->setState(UserConnection::STATE_LOCK);
}

void ConnectionManager::on(UserConnectionListener::CLock, UserConnection* aSource, const string& aLock, const string& aPk) noexcept
{
	if (aSource->getState() != UserConnection::STATE_LOCK)
	{
		dcdebug("CM::onLock %p received lock twice, ignoring\n", (void*)aSource);
		return;
	}
	
	if (CryptoManager::getInstance()->isExtended(aLock))
	{
		StringList defFeatures = features;
		if (BOOLSETTING(COMPRESS_TRANSFERS))
		{
			defFeatures.push_back(UserConnection::FEATURE_ZLIB_GET);
		}
		
		aSource->supports(defFeatures);
	}
	
	aSource->setState(UserConnection::STATE_DIRECTION);
	aSource->direction(aSource->getDirectionString(), aSource->getNumber());
	aSource->key(CryptoManager::getInstance()->makeKey(aLock));
	
	if (aSource->getUser())
		ClientManager::getInstance()->setPkLock(aSource->getUser(), aPk, aLock);
}

void ConnectionManager::on(UserConnectionListener::Direction, UserConnection* aSource, const string& dir, const string& num) noexcept
{
	if (aSource->getState() != UserConnection::STATE_DIRECTION)
	{
		dcdebug("CM::onDirection %p received direction twice, ignoring\n", (void*)aSource);
		return;
	}
	
	dcassert(aSource->isSet(UserConnection::FLAG_DOWNLOAD) ^ aSource->isSet(UserConnection::FLAG_UPLOAD));
	if (dir == "Upload")
	{
		// Fine, the other fellow want's to send us data...make sure we really want that...
		if (aSource->isSet(UserConnection::FLAG_UPLOAD))
		{
			// Huh? Strange...disconnect...
			putConnection(aSource);
			return;
		}
	}
	else {
		if (aSource->isSet(UserConnection::FLAG_DOWNLOAD))
		{
			int number = Util::toInt(num);
			// Damn, both want to download...the one with the highest number wins...
			if (aSource->getNumber() < number)
			{
				// Damn! We lost!
				aSource->unsetFlag(UserConnection::FLAG_DOWNLOAD);
				aSource->setFlag(UserConnection::FLAG_UPLOAD);
			}
			else if (aSource->getNumber() == number)
			{
				putConnection(aSource);
				return;
			}
		}
	}
	
	dcassert(aSource->isSet(UserConnection::FLAG_DOWNLOAD) ^ aSource->isSet(UserConnection::FLAG_UPLOAD));
	
	aSource->setState(UserConnection::STATE_KEY);
}
void ConnectionManager::store_last_ip(UserConnection* p_uc, ConnectionQueueItem* p_qi)
{
	if (BOOLSETTING(ENABLE_LAST_IP) && p_uc && p_qi)
	{
		CFlylinkDBManager::getInstance()->store_last_ip(
		    p_uc->getHubUrl(),
		    p_qi->getUser().user->getFirstNick(),
		    p_uc->getSocket()->getIp());
		p_qi->getUser().user->setLastIP(p_uc->getSocket()->getIp());
	}
}
void ConnectionManager::addDownloadConnection(UserConnection* uc)
{
	dcassert(uc->isSet(UserConnection::FLAG_DOWNLOAD));
	ConnectionQueueItem* cqi = nullptr;
	{
		Lock l(cs);
		
		ConnectionQueueItem::Iter i = find(downloads.begin(), downloads.end(), uc->getUser());
		if (i != downloads.end())
		{
			cqi = *i;
			if (cqi->getState() == ConnectionQueueItem::WAITING || cqi->getState() == ConnectionQueueItem::CONNECTING)
			{
				cqi->setState(ConnectionQueueItem::ACTIVE);
				uc->setFlag(UserConnection::FLAG_ASSOCIATED);
				
#ifdef FLYLINKDC_USE_CONNECTED_EVENT
				fire(ConnectionManagerListener::Connected(), cqi);
#endif
				
				dcdebug("ConnectionManager::addDownloadConnection, leaving to downloadmanager\n");
			}
			else
				cqi = nullptr;
		}
	}
	
	if (cqi)
	{
		DownloadManager::getInstance()->addConnection(uc);
		store_last_ip(uc, cqi); //[+]PPA
	}
	else
	{
		putConnection(uc);
	}
}

void ConnectionManager::addUploadConnection(UserConnection* uc)
{
	dcassert(uc->isSet(UserConnection::FLAG_UPLOAD));
	
	ConnectionQueueItem* cqi = nullptr;
	{
		Lock l(cs);
		
		ConnectionQueueItem::Iter i = find(uploads.begin(), uploads.end(), uc->getUser());
		if (i == uploads.end())
		{
			cqi = getCQI(uc->getHintedUser(), false);
			cqi->setState(ConnectionQueueItem::ACTIVE);
			uc->setFlag(UserConnection::FLAG_ASSOCIATED);
#ifdef FLYLINKDC_USE_CONNECTED_EVENT
			fire(ConnectionManagerListener::Connected(), cqi);
#endif
			dcdebug("ConnectionManager::addUploadConnection, leaving to uploadmanager\n");
		}
	}
	
	if (cqi)
	{
		UploadManager::getInstance()->addConnection(uc);
		store_last_ip(uc, cqi); //[+]PPA
	}
	else
	{
		putConnection(uc);
	}
}

void ConnectionManager::on(UserConnectionListener::Key, UserConnection* aSource, const string&/* aKey*/) noexcept
{
	if (aSource->getState() != UserConnection::STATE_KEY)
	{
		dcdebug("CM::onKey Bad state, ignoring");
		return;
	}
	
	dcassert(aSource->getUser());
	
	if (aSource->isSet(UserConnection::FLAG_DOWNLOAD))
	{
		addDownloadConnection(aSource);
	}
	else {
		addUploadConnection(aSource);
	}
}

void ConnectionManager::on(AdcCommand::INF, UserConnection* aSource, const AdcCommand& cmd) noexcept
{
	if (aSource->getState() != UserConnection::STATE_INF)
	{
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_PROTOCOL_GENERIC, "Expecting INF"));
		aSource->disconnect();
		return;
	}
	
	string cid;
	if (!cmd.getParam("ID", 0, cid))
	{
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_INF_MISSING, "ID missing").addParam("FL", "ID"));
		dcdebug("CM::onINF missing ID\n");
		aSource->disconnect();
		return;
	}
	
	aSource->setUser(ClientManager::getInstance()->findUser(CID(cid)));
	
	if (!aSource->getUser())
	{
		dcdebug("CM::onINF: User not found");
		aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_GENERIC, "User not found"));
		putConnection(aSource);
		return;
	}
	
	if (!checkKeyprint(aSource))
	{
		putConnection(aSource);
		return;
	}
	
	string token;
	if (aSource->isSet(UserConnection::FLAG_INCOMING))
	{
		if (!cmd.getParam("TO", 0, token))
		{
			aSource->send(AdcCommand(AdcCommand::SEV_FATAL, AdcCommand::ERROR_GENERIC, "TO missing"));
			putConnection(aSource);
			return;
		}
	}
	else {
		token = aSource->getToken();
	}
	
	bool down = false;
	{
		Lock l(cs);
		ConnectionQueueItem::Iter i = find(downloads.begin(), downloads.end(), aSource->getUser());
		
		if (i != downloads.end())
		{
			(*i)->setErrors(0);
			
			const string& to = (*i)->getToken();
			
			if (to == token)
			{
				down = true;
			}
		}
		/** @todo check tokens for upload connections */
	}
	
	if (down)
	{
		aSource->setFlag(UserConnection::FLAG_DOWNLOAD);
		addDownloadConnection(aSource);
	}
	else {
		aSource->setFlag(UserConnection::FLAG_UPLOAD);
		addUploadConnection(aSource);
	}
}

void ConnectionManager::force(const UserPtr& aUser)
{
	Lock l(cs);
	
	ConnectionQueueItem::Iter i = find(downloads.begin(), downloads.end(), aUser);
	if (i == downloads.end())
	{
		return;
	}
	
	(*i)->setLastAttempt(0);
}

bool ConnectionManager::checkKeyprint(UserConnection *aSource)
{
	dcassert(aSource->getUser());
	
	auto kp = aSource->getKeyprint();
	if (kp.empty())
	{
		return true;
	}
	
	auto kp2 = ClientManager::getInstance()->getField(aSource->getUser()->getCID(), aSource->getHubUrl(), "KP");
	if (kp2.empty())
	{
		// TODO false probably
		return true;
	}
	
	if (kp2.compare(0, 7, "SHA256/") != 0)
	{
		// Unsupported hash
		return true;
	}
	
	dcdebug("Keyprint: %s vs %s\n", Encoder::toBase32(&kp[0], kp.size()).c_str(), kp2.c_str() + 7);
	
	vector<uint8_t> kp2v(kp.size());
	Encoder::fromBase32(&kp2[7], &kp2v[0], kp2v.size());
	if (!std::equal(kp.begin(), kp.end(), kp2v.begin()))
	{
		dcdebug("Not equal...\n");
		return false;
	}
	
	return true;
}

void ConnectionManager::failed(UserConnection* aSource, const string& aError, bool protocolError)
{
	Lock l(cs);
	
	if (aSource->isSet(UserConnection::FLAG_ASSOCIATED))
	{
		if (aSource->isSet(UserConnection::FLAG_DOWNLOAD))
		{
			ConnectionQueueItem::Iter i = find(downloads.begin(), downloads.end(), aSource->getUser());
			dcassert(i != downloads.end());
			ConnectionQueueItem* cqi = *i;
			cqi->setState(ConnectionQueueItem::WAITING);
			cqi->setLastAttempt(GET_TICK());
			cqi->setErrors(protocolError ? -1 : (cqi->getErrors() + 1));
			fire(ConnectionManagerListener::Failed(), cqi, aError);
		}
		else if (aSource->isSet(UserConnection::FLAG_UPLOAD))
		{
			ConnectionQueueItem::Iter i = find(uploads.begin(), uploads.end(), aSource->getUser());
			dcassert(i != uploads.end());
			ConnectionQueueItem* cqi = *i;
			putCQI(cqi);
		}
	}
	putConnection(aSource);
}

void ConnectionManager::on(UserConnectionListener::Failed, UserConnection* aSource, const string& aError) noexcept
{
	failed(aSource, aError, false);
}

void ConnectionManager::on(UserConnectionListener::ProtocolError, UserConnection* aSource, const string& aError) noexcept
{
	failed(aSource, aError, true);
}

void ConnectionManager::disconnect(const UserPtr& aUser)
{
	Lock l(cs);
	for (UserConnectionList::const_iterator i = userConnections.begin(); i != userConnections.end(); ++i)
	{
		UserConnection* uc = *i;
		if (uc->getUser() == aUser)
			uc->disconnect(true);
	}
}

void ConnectionManager::disconnect(const UserPtr& aUser, int isDownload)
{
	Lock l(cs);
	for (UserConnectionList::const_iterator i = userConnections.begin(); i != userConnections.end(); ++i)
	{
		UserConnection* uc = *i;
		if (uc->getUser() == aUser && uc->isSet((Flags::MaskType)(isDownload ? UserConnection::FLAG_DOWNLOAD : UserConnection::FLAG_UPLOAD)))
		{
			uc->disconnect(true);
			break;
		}
	}
}

void ConnectionManager::shutdown()
{
	TimerManager::getInstance()->removeListener(this);
	shuttingDown = true;
	disconnect();
	{
		Lock l(cs);
		for (UserConnectionList::const_iterator j = userConnections.begin(); j != userConnections.end(); ++j)
		{
			(*j)->disconnect(true);
		}
	}
	// Wait until all connections have died out...
	while (true)
	{
		{
			Lock l(cs);
			if (userConnections.empty())
			{
				break;
			}
		}
		Thread::sleep(50);
	}
}

// UserConnectionListener
void ConnectionManager::on(UserConnectionListener::Supports, UserConnection* conn, const StringList& feat) noexcept
{
	string sup;
	for (StringList::const_iterator i = feat.begin(); i != feat.end(); ++i)
	{
		sup += (string) * i + " ";
		if (*i == UserConnection::FEATURE_MINISLOTS)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_MINISLOTS);
		}
		else if (*i == UserConnection::FEATURE_XML_BZLIST)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_XML_BZLIST);
		}
		else if (*i == UserConnection::FEATURE_ADCGET)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_ADCGET);
		}
		else if (*i == UserConnection::FEATURE_ZLIB_GET)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_ZLIB_GET);
		}
		else if (*i == UserConnection::FEATURE_TTHL)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_TTHL);
		}
		else if (*i == UserConnection::FEATURE_TTHF)
		{
			conn->setFlag(UserConnection::FLAG_SUPPORTS_TTHF);
		}
	}
	
	if (conn->getUser())
		ClientManager::getInstance()->setSupports(conn->getUser(), sup);
}

} // namespace dcpp

/**
 * @file
 * $Id: ConnectionManager.cpp 575 2011-08-25 19:38:04Z bigmuscle $
 */