/*
Copyright (C) 2026 Miranda NG team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2
of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

// The connection, as Jasmine IM does it against kicq.ru:
//   connect login server -> server hello (FLAP ch1) -> XOR login (ch1)
//   -> ch4 with BOS address + cookie -> reconnect to BOS -> hello -> cookie (ch1)
//   -> 01/03 families -> 01/17 versions -> 01/18 -> 01/06 rates -> 01/07 -> 01/08 ack,
//      01/0E, 13/02, 02/02, 03/02, 04/04, 09/02, 13/04 roster request
//   -> 13/06 roster -> 02/04 caps, 04/02 icbm, 01/02 client ready, 01/1E status,
//      13/07 activate, 15/02 offline messages
//   -> online: 03/0B,0C presence; 04/07 messages; 04/0B acks; 04/14 typing; 13/19,1B auth ...

#include "stdafx.h"
#include "bart_wire.h"
#include "bart_codec.h"
#include "bart_avatar_store.h"
#include "bart_ssi.h"
#include <m_avatars.h>
#include "oft2_rendezvous.h"
#include "xstatus_catalog.h"

// Used by both incoming Xtraz requests and responses.
static bytes_t KicqUnescapeXml(bytes_t text);
// Targeted Extra Icon refresh implemented in proto.cpp.
void KicqRefreshSingleXStatus(MCONTACT hContact);

#define KEEPALIVE_INTERVAL 60
#define INFO_TIMEOUT 8


// Standalone Xtraz diagnostic file, independent of Netlib Log.
// Location: %TEMP%\\PMLICQ-Xtraz-Debug.txt (Windows user temp directory).
// Only the Xtraz message branch calls this function; no credentials or chat text
// are intentionally collected. The packet itself can contain personal IDs.
static void KicqWriteXtrazDebug(const char *line)
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n >= MAX_PATH)
        return;
    static const wchar_t filename[] = L"PMLICQ-Xtraz-Debug.txt";
    if (n + _countof(filename) > MAX_PATH)
        return;
    wcscat_s(path, filename);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME now;
    GetLocalTime(&now);
    char stamp[64];
    mir_snprintf(stamp, "[%04u-%02u-%02u %02u:%02u:%02u] ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    DWORD written = 0;
    WriteFile(file, stamp, (DWORD)strlen(stamp), &written, nullptr);
    WriteFile(file, line, (DWORD)strlen(line), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

// XP diagnostic trace: records stages only, no UINs, message bodies or credentials.
// File: %TEMP%\PMLICQ-XP-Connection-Trace.txt
static void KicqXpTrace(const char *stage)
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetTempPathW(_countof(path), path);
    if (!len || len >= _countof(path)) return;
    const wchar_t *filename = L"PMLICQ-XP-Connection-Trace.txt";
    if (len + wcslen(filename) + 1 >= _countof(path)) return;
    wcscat_s(path, filename);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[160];
    int n = mir_snprintf(line, "tick=%lu thread=%lu %s\r\n",
        (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), stage);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
    }
    CloseHandle(h);
}

static int LoginErrorToMiranda(int code)
{
	switch (code) {
	case 0x01: case 0x07: case 0x08:
		return LOGINERR_BADUSERID;
	case 0x04: case 0x05:
		return LOGINERR_WRONGPASSWORD;
	case 0x18: case 0x1D:
		return LOGINERR_TIMEOUT;
	}
	return LOGINERR_WRONGPROTOCOL;
}

/////////////////////////////////////////////////////////////////////////////////////////
// the session thread

void CKicqProto::ServerThread(void *)
{
	KicqXpTrace("ServerThread begin");
	m_iLoginError = 0;
	m_cookie.clear();
	m_szUin.Format("%u", getDword("UIN"));
	{
		mir_cslock lck(m_csRoster);
		m_arUsers.destroy();
		m_arGroups.destroy();
		m_offlineSeen.clear();
	}
	{
		mir_cslock lck(m_csPending);
		m_pendingMsgs.clear();
		m_infoRequests.clear();
	}

	T2Utf szServer(m_wszServer);
	int port = m_iPort;
	if (port <= 0)
		port = KICQ_DEFAULT_PORT;

	debugLogA("connecting to %s:%d as %s", szServer.get(), port, m_szUin.c_str());
	if (RunConnection(szServer, port, true) && !m_bTerminated && !m_cookie.empty())
		{
		// Some self-hosted OSCAR servers advertise a BOS hostname that is only
		// resolvable inside their container or private network.
		CMStringA bosHost = m_szBosHost;
		int bosPort = m_iBosPort;
		if (m_iBosMode == 1)
			bosHost = szServer.get();
		else if (m_iBosMode == 2) {
			T2Utf configuredHost(m_wszBosServer);
			if (configuredHost.get() && *configuredHost.get())
				bosHost = configuredHost.get();
		}
		if (m_iBosPortOverride > 0 && m_iBosPortOverride <= 65535)
			bosPort = m_iBosPortOverride;
		debugLogA("connecting to BOS %s:%d (mode %d)", bosHost.c_str(), bosPort, (int)m_iBosMode);
		RunConnection(bosHost, bosPort, false);
	}

	debugLogA("session finished");
	m_bOnline = false;

	int iOldStatus = m_iStatus;
	m_iStatus = m_iDesiredStatus = ID_STATUS_OFFLINE;
	setAllContactStatuses(ID_STATUS_OFFLINE);
	// The server session ended: discard remote presence and cached X-status
	// for this account, including contacts without a USER_OFFLINE packet.
	for (MCONTACT h = db_find_first(); h; h = db_find_next(h)) {
		const char *module = Proto_GetBaseAccountName(h);
		if (module && !mir_strcmp(module, m_szModuleName)) {
			db_unset(h, m_szModuleName, "XStatusId");
			db_unset(h, m_szModuleName, "XStatusName");
			db_unset(h, m_szModuleName, "XStatusLive");
			KicqRefreshSingleXStatus(h);
		}
	}
	if (m_iLoginError)
		ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, m_iLoginError);
	ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)iOldStatus, m_iStatus);
}

// one TCP connection (the login server or the BOS); returns true when it ended the way it
// should: with a redirect for the login server, or a normal close of the BOS session
bool CKicqProto::RunConnection(const char *host, int port, bool bLogin)
{
	KicqXpTrace(bLogin ? "login TCP connect begin" : "BOS TCP connect begin");
	HNETLIBCONN hConn = Netlib_OpenConnection(m_hNetlibUser, host, port, 20);
	if (hConn == nullptr) {
		debugLogA("cannot connect to %s:%d", host, port);
		m_iLoginError = LOGINERR_NONETWORK;
		return false;
	}
	{
		mir_cslock lck(m_csSend);
		m_hConn = hConn;
		m_seq = (rand() & 0x7FFF) + 1;
	}

	KicqXpTrace(bLogin ? "login TCP connected" : "BOS TCP connected");
	ConnResult res = CONN_CLOSE;
	bytes_t inbound;
	char buf[8192];

	while (!m_bTerminated) {
		NETLIBSELECT nls = {};
		nls.dwTimeout = 1000;
		nls.hReadConns[0] = hConn;
		int ret = Netlib_Select(&nls);
		if (m_bTerminated || ret < 0)
			break;
		if (ret == 0) {
			CheckTimeouts();
			continue;
		}

		int n = Netlib_Recv(hConn, buf, sizeof(buf), MSG_NODUMP);
		if (n <= 0) {
			debugLogA("connection closed by server");
			break;
		}
		inbound.append(buf, n);

		res = CONN_CONTINUE;
		while (inbound.size() >= 6 && res == CONN_CONTINUE) {
			if ((uint8_t)inbound[0] != 0x2A) {
				debugLogA("protocol error: not a FLAP frame");
				res = CONN_CLOSE;
				break;
			}
			int channel = (uint8_t)inbound[1];
			int len = ((uint8_t)inbound[4] << 8) | (uint8_t)inbound[5];
			if ((int)inbound.size() < 6 + len)
				break;

			bytes_t data = inbound.substr(6, len);
			inbound.erase(0, 6 + len);
			Netlib_Dump(hConn, data.data(), data.size(), false, 0);
			res = HandleFlap(channel, data, bLogin);
		}
		if (res != CONN_CONTINUE)
			break;
		CheckTimeouts();
	}

	{
		mir_cslock lck(m_csSend);
		m_hConn = nullptr;
	}
	Netlib_CloseHandle(hConn);
	m_bOnline = false;
	return bLogin ? (res == CONN_REDIRECT) : true;
}

void CKicqProto::ShutdownSession()
{
	m_bTerminated = true;

	mir_cslock lck(m_csSend);
	if (m_hConn) {
		bytes_t bye = OscarPackets::flap(4, m_seq++, bytes_t());
		// A short send is possible even on a blocking TCP socket.
		for (size_t sent = 0; sent < bye.size();) {
			int n = Netlib_Send(m_hConn, bye.data() + sent, (int)(bye.size() - sent), MSG_NODUMP);
			if (n <= 0) break;
			sent += n;
		}
		Netlib_Shutdown(m_hConn);
	}
}

void CKicqProto::Send(int channel, const bytes_t &payload)
{
	mir_cslock lck(m_csSend);
	if (m_hConn == nullptr)
		return;

	bytes_t frame = OscarPackets::flap(channel, m_seq, payload);
	m_seq = (m_seq + 1) & 0xFFFF;
	if (channel == 2) // the login packets carry the password
		Netlib_Dump(m_hConn, payload.data(), payload.size(), true, 0);
	// Netlib_Send is not guaranteed to transmit the entire FLAP in one call.
	for (size_t sent = 0; sent < frame.size();) {
		int n = Netlib_Send(m_hConn, frame.data() + sent, (int)(frame.size() - sent), MSG_NODUMP);
		if (n <= 0) {
			debugLogA("FLAP send failed: channel=%d, sent=%u/%u", channel,
				(unsigned)sent, (unsigned)frame.size());
			m_bTerminated = true;
			Netlib_Shutdown(m_hConn);
			break;
		}
		sent += n;
	}
}

void CKicqProto::Send(const OscarSnac &snac)
{
	Send(2, OscarPackets::snacBytes(snac));
}

void CKicqProto::CheckTimeouts()
{
	time_t now = time(0);

	if (m_bOnline && now - m_lastProbe >= KEEPALIVE_INTERVAL) {
		// OSCAR transport keepalive is FLAP channel 5 (empty payload).
		// A keepalive does not require a SNAC reply: disconnecting after one
		// unanswered application-level probe causes false session timeouts.
		m_lastProbe = now;
		Send(5, bytes_t());
	}

	// Poll the text separately from the X-status capability GUID. QIP/ICQ 6
	// may change only title/description without broadcasting new presence.
	// CheckTimeouts runs on the network thread; do not create a UI timer.
	// Bound each pass to avoid a burst for a large contact list.
	// On Windows XP, limit DB scanning/query traffic to avoid starving the contact list.
	if (m_bOnline) {
		int sent = 0;
		for (MCONTACT h = db_find_first(); h && sent < 2; h = db_find_next(h)) {
			const char *module = Proto_GetBaseAccountName(h);
			if (!module || mir_strcmp(module, m_szModuleName))
				continue;
			if (!getByte(h, "XStatusLive", 0) ||
				getWord(h, "Status", ID_STATUS_OFFLINE) == ID_STATUS_OFFLINE)
				continue;
			const uint32_t previous = getDword(h, "XStatusQueryTS", 0);
			const uint32_t current = uint32_t(now);
			if (previous && current >= previous && current - previous < 60)
				continue;
			ptrA uinText(getStringA(h, "UIN"));
			const uint32_t uin = uinText ? uint32_t(atoi(uinText.get())) : 0;
			if (!uin) continue;
			setDword(h, "XStatusQueryTS", current);
			OscarWriter cookie;
			cookie.u32(current).u32(uin);
			CMStringA target(FORMAT, "%u", uin);
			Send(OscarPackets::xtrazAwayQuery(cookie.data(), target.c_str(), m_szUin.c_str()));
			++sent;
		}
	}

	// whatever arrived is delivered: a server that does not know a chunk never sends it
	std::vector<KicqInfoRequest> expired;
	{
		mir_cslock lck(m_csPending);
		for (auto it = m_infoRequests.begin(); it != m_infoRequests.end();) {
			if (now - it->second.started >= INFO_TIMEOUT) {
				expired.push_back(it->second);
				it = m_infoRequests.erase(it);
			}
			else ++it;
		}
	}

	for (auto &req : expired) {
		if (req.hSearch) {
			CMStringW wszUin(FORMAT, L"%u", req.uin);
			PROTOSEARCHRESULT psr = {};
			psr.cbSize = sizeof(psr);
			psr.flags = PSR_UNICODE;
			psr.id.w = wszUin.GetBuffer();
			psr.nick.w = req.nick.GetBuffer();
			psr.firstName.w = req.firstName.GetBuffer();
			psr.lastName.w = req.lastName.GetBuffer();
			psr.email.w = req.email.GetBuffer();
			ProtoBroadcastAck(0, ACKTYPE_SEARCH, ACKRESULT_DATA, req.hSearch, (LPARAM)&psr);
			ProtoBroadcastAck(0, ACKTYPE_SEARCH, ACKRESULT_SUCCESS, req.hSearch, 0);
		}
		else ProtoBroadcastAck(req.hContact, ACKTYPE_GETINFO, ACKRESULT_SUCCESS, (HANDLE)1, 0);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// FLAP level

CKicqProto::ConnResult CKicqProto::HandleFlap(int channel, const bytes_t &data, bool bLogin)
{
	switch (channel) {
	case 1:
		if (bLogin) {
			debugLogA("login server hello, sending XOR login");
			ptrA szPassword(getStringA("Password"));
			Send(1, OscarPackets::xorLogin(m_szUin, szPassword ? szPassword.get() : ""));
		}
		else {
			debugLogA("BOS hello, sending cookie");
			Send(1, OscarPackets::bosCookie(m_cookie));
		}
		break;

	case 2:
		HandleSnac(data);
		break;

	case 4:
		return HandleChannel4(data);
	}
	return CONN_CONTINUE;
}

CKicqProto::ConnResult CKicqProto::HandleChannel4(const bytes_t &data)
{
	OscarTlvList tlvs(data);
	auto *server = tlvs.find(5), *cookie = tlvs.find(6);
	if (server && cookie) {
		CMStringA bos(server->data.c_str());
		int idx = bos.Find(':');
		m_szBosHost = (idx == -1) ? bos : bos.Left(idx);
		m_iBosPort = (idx == -1) ? 0 : atoi(bos.Mid(idx + 1));
		if (m_iBosPort <= 0)
			m_iBosPort = m_iPort;
		m_cookie = cookie->data;
		debugLogA("login accepted, BOS at %s:%d", m_szBosHost.c_str(), m_iBosPort);
		Send(4, bytes_t());
		return CONN_REDIRECT;
	}

	if (auto *error = tlvs.find(8)) {
		int code = error->u16();
		debugLogA("login refused, code %d", code);
		m_iLoginError = LoginErrorToMiranda(code);
		return CONN_CLOSE;
	}

	auto *t9 = tlvs.find(9);
	if (t9 && t9->u16() == 1) {
		debugLogA("this UIN signed on from another place");
		m_iLoginError = LOGINERR_OTHERLOCATION;
	}
	else debugLogA("server closed the session");
	return CONN_CLOSE;
}

/////////////////////////////////////////////////////////////////////////////////////////
// SNAC dispatch

void CKicqProto::HandleSnac(const bytes_t &data)
{
	if (data.size() < 10)
		return;

	OscarReader r(data);
	int family = r.u16();
	int subtype = r.u16();
	int flags = r.u16();
	uint32_t reqId = r.u32();
	bytes_t body = r.rest();
	m_bProbeAnswered = true;

	// an optional block of extra data precedes the payload when bit 15 of the flags is set
	if (flags & 0x8000) {
		OscarReader e(body);
		e.skip(e.u16());
		body = e.rest();
	}

	switch (family) {
	case 0x01:
		switch (subtype) {
		case 0x05: HandleBartRedirect(body); break;
        case 0x21: { // Server requests own BART upload after SSI publication.
         bart::AssetId id; size_t offset = 0;
         if (bart::parseId(body, offset, id) && id.type == 1 && (id.flags & 0x40)) {
          mir_cslock lock(m_csBart);
          if (id.hash == m_ownIconPendingHash && !m_ownIconPendingUpload.empty()) {
           bytes_t payload, wire;
           if (bart::makeUpload(1, m_ownIconPendingUpload, payload) &&
               bart::makeSnac({0x10, 0x02, 0, 0, payload}, wire)) m_bartOutgoing.push_back(wire);
          }
         }
         break;
        }
		case 0x03: case 0x15: Send(OscarPackets::clientFamilies()); break;
		case 0x18: Send(OscarPackets::ratesRequest()); break;
		case 0x07: HandleRates(body); break;
		}
		break;

	case 0x03:
		if (subtype == 0x0B)
			HandleUserOnline(body);
		else if (subtype == 0x0C)
			HandleUserOffline(body);
		break;

	case 0x04:
		switch (subtype) {
		case 0x01: HandleMessageError(body, reqId); break;
		case 0x07: HandleIncomingMessage(body); break;
		case 0x0B: case 0x0C: HandleMessageAck(body); break;
		case 0x14: HandleTyping(body); break;
		}
		break;

	case 0x13:
		switch (subtype) {
		case 0x06: HandleRoster(body, flags); break;
		case 0x08: case 0x09: HandleSsiItem(body, false); break;
		case 0x0A: HandleSsiItem(body, true); break;
		case 0x0E: HandleSsiResult(body, reqId); break;
		case 0x19: HandleAuthRequest(body); break;
		case 0x1B: HandleAuthReply(body); break;
		case 0x1C: HandleYouWereAdded(body); break;
		}
		break;

	case 0x15:
		if (subtype == 0x03) {
			if (reqId == REQ_OFFLINE_MSGS)
				HandleOfflineMessage(body);
			else
				HandleUserInfo(body, reqId);
		}
		break;
	}
}

// BOS service redirect for BART, SNAC(1,5). The advertised cookie is
// separate from the BOS login cookie and must not overwrite m_cookie.
void CKicqProto::HandleBartRedirect(const bytes_t &data)
{
 OscarTlvList tlvs(data);
 const auto *family = tlvs.find(0x000D);
 const auto *server = tlvs.find(0x0005);
 const auto *cookie = tlvs.find(0x0006);
 if (!family || family->data.size() != 2 || family->u16() != 0x0010 ||
     !server || server->data.empty() || !cookie || cookie->data.empty()) {
  debugLogA("BART: ignoring incomplete service redirect");
  return;
 }
 CMStringA address(server->data.c_str());
 int colon = address.ReverseFind(':');
 m_bartHost = colon < 0 ? address : address.Left(colon);
 m_bartPort = colon < 0 ? 5190 : atoi(address.Mid(colon + 1));
 if (m_bartHost.IsEmpty() || m_bartPort < 1 || m_bartPort > 65535) {
  m_bartHost.Empty();
  m_bartCookie.clear();
  debugLogA("BART: invalid redirect endpoint");
  return;
 }
 m_bartCookie = cookie->data;
 debugLogA("BART: server redirect to %s:%d received", m_bartHost.c_str(), m_bartPort);
 // A finished worker still has a non-null HANDLE. Reap it before starting
 // a fresh BART session with the new (single-use) redirect cookie.
 if (m_hBartThread && WaitForSingleObject(m_hBartThread, 0) == WAIT_OBJECT_0) {
  CloseHandle(m_hBartThread);
  m_hBartThread = nullptr;
  debugLogA("BART: previous service worker finished; restarting");
 }
 if (!m_hBartThread && !m_bTerminated) {
  InterlockedExchange(&m_bartStop, 0);
  m_hBartThread = CreateThread(nullptr, 0, BartThreadEntry, this, 0, nullptr);
  if (!m_hBartThread) debugLogA("BART: failed to start service thread");
 }
}

void CKicqProto::HandleRates(const bytes_t &data)
{
	int groups = OscarReader(data).u16();
	debugLogA("rates: %d classes; requesting roster", groups);
	Send(OscarPackets::ackRates(groups));
	Send(OscarPackets::reqSelfInfo());
	Send(OscarPackets::reqSsiRights());
	Send(OscarPackets::reqLocationRights());
	Send(OscarPackets::reqBuddyRights());
	Send(OscarPackets::reqIcbmParams());
	Send(OscarPackets::reqPrivacyRights());
	Send(OscarPackets::requestRoster());
	// Request BART service endpoint from BOS (SNAC 1/04).
	bytes_t bartFamily; bartFamily.push_back(0); bartFamily.push_back(0x10);
	Send(OscarSnac(1, 4, 0, bartFamily));
}

/////////////////////////////////////////////////////////////////////////////////////////
// roster

// mirrors one server-side contact into the contact list
void CKicqProto::ApplyRosterItem(uint32_t uin, int groupId, int ssiId, const OscarTlvList &tlvs, bool bFromLogin)
{
	bool bTemporary;
	CMStringW wszGroup;
	{
		mir_cslock lck(m_csRoster);
		auto *pGroup = FindGroup(groupId);
		bTemporary = pGroup && pGroup->bNotInList;
		if (pGroup && !pGroup->bNotInList)
			wszGroup = pGroup->name;
	}

	MCONTACT hContact = FindContact(uin);
	bool bNew = (hContact == 0);
	if (bNew)
		hContact = CreateContact(uin, bTemporary);

	bool bAuthorized = !tlvs.has(0x66);
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = AddUser(uin, hContact);
		pUser->hContact = hContact;
		pUser->groupId = groupId;
		pUser->ssiId = bTemporary ? 0 : ssiId;
		pUser->bAuthorized = bAuthorized;
	}

	// Avoid redundant database notifications for every SSI item on login.
	if (bAuthorized) {
		if (getByte(hContact, "Auth", 0))
			delSetting(hContact, "Auth");
	}
	else if (getByte(hContact, "Auth", 0) != 1)
		setByte(hContact, "Auth", 1);

	if (bTemporary)
		return;

	// A redundant PutOnList/Hide during the initial roster download
	// triggers needless clist rebuilds on older Windows XP installations.
	if (db_get_b(hContact, "CList", "NotOnList", 0))
		Contact::PutOnList(hContact);
	if ((bNew || !bFromLogin) && db_get_b(hContact, "CList", "Hidden", 0))
		Contact::Hide(hContact, false);

	if (auto *nick = tlvs.find(0x131)) {
		CMStringW wszNick(OscarText::textOrCp1251(nick->data));
		if (!wszNick.IsEmpty()) {
			// SrvNick goes first: OnSettingChanged compares MyHandle with it to skip the echo
			if (getMStringW(hContact, "SrvNick") != wszNick)
				setWString(hContact, "SrvNick", wszNick);
			if (db_get_wsm(hContact, "CList", "MyHandle") != wszNick)
				db_set_ws(hContact, "CList", "MyHandle", wszNick);
			if (getMStringW(hContact, "Nick").IsEmpty())
				setWString(hContact, "Nick", wszNick);
		}
	}

	if (!wszGroup.IsEmpty()) {
		ptrW wszOld(Clist_GetGroup(hContact));
		if (mir_wstrcmp(wszOld, wszGroup)) {
			KicqXpTrace("Clist_GroupCreate before");
			Clist_GroupCreate(0, wszGroup);
			KicqXpTrace("Clist_SetGroup before");
			Clist_SetGroup(hContact, wszGroup);
			KicqXpTrace("Clist_SetGroup after");
		}
	}
}

void CKicqProto::HandleRoster(const bytes_t &data, int flags)
{
	KicqXpTrace("HandleRoster begin");
	struct Item { CMStringA name; int groupId, itemId; bytes_t extra; };
	std::vector<Item> contacts;

	OscarReader r(data);
	r.skip(1); // SSI version
	int count = r.u16();
	for (int i = 0; i < count && !r.atEnd(); i++) {
		bytes_t rawName = r.bytes(r.u16());
		int groupId = r.u16();
		int itemId = r.u16();
		int type = r.u16();
		bytes_t extra = r.bytes(r.u16());

		if (type == 0 && groupId != 0)
			contacts.push_back({ rawName.c_str(), groupId, itemId, extra });
		else if (type == 0x14 && groupId == 0) {
            m_ownIconItemId = uint16_t(itemId);
            setWord("BartOwnIconItemId", m_ownIconItemId);
            // Restore the icon advertised by the server, including one set by QIP.
            // SSI TLV 0x00d5 contains: flags(1), hash-length(1), MD5(16).
            OscarTlvList iconTlvs(extra);
            if (const auto *icon = iconTlvs.find(0x00d5)) {
                const bytes_t &v = icon->data;
                if (v.size() == 18 && uint8_t(v[1]) == 16) {
                    const bytes_t hash = v.substr(2, 16);
                    m_ownIconServerHash = hash;
                    const auto oldHash = getMStringA("BartOwnAvatarHash");
                    const auto oldPath = getMStringW("BartOwnAvatarPath");
                    if (oldHash != bart::hexHash(hash).c_str() || oldPath.IsEmpty() ||
                        GetFileAttributesW(oldPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        delSetting("BartOwnAvatarPath");
                        ReportSelfAvatarChanged();
                        QueueBartAvatar(uint32_t(atoi(m_szUin)), 1, hash);
                    }
                }
            }
        }
		else if (type == 1 && groupId != 0) {
			OscarTlvList tlvs(extra);
			mir_cslock lck(m_csRoster);
			auto *pGroup = FindGroup(groupId);
			if (pGroup == nullptr)
				m_arGroups.insert(pGroup = new KicqGroup());
			pGroup->id = groupId;
			pGroup->name = OscarText::textOrCp1251(rawName);
			pGroup->bNotInList = tlvs.has(0x6A);
		}
		// types 2/3/14 (visible/invisible/ignore lists), 4 (privacy), 20 (icon) are not used
	}

	KicqXpTrace("HandleRoster parsed; applying contacts");
	// groups may come after their contacts, so contacts are applied once the packet is read
	size_t xpRosterIndex = 0;
	for (auto &it : contacts) {
		if ((xpRosterIndex++ % 25) == 0) KicqXpTrace("HandleRoster contacts batch begin");
		uint32_t uin = strtoul(it.name, nullptr, 10);
		if (uin)
			ApplyRosterItem(uin, it.groupId, it.itemId, OscarTlvList(it.extra), true);
	}

	KicqXpTrace("HandleRoster contacts applied");
	if ((flags & 0x0001) == 0) {
		debugLogA("roster: %d groups, %d contacts", m_arGroups.getCount(), m_arUsers.getCount());
		FinishLogin();
	}
}

void CKicqProto::FinishLogin()
{
	KicqXpTrace("FinishLogin begin");
	int iOldStatus = m_iStatus;
	m_iStatus = m_iDesiredStatus;
	if (m_iStatus < ID_STATUS_ONLINE)
		m_iStatus = ID_STATUS_ONLINE;

	auto *pVer = &pluginInfoEx.version;
	Send(OscarPackets::setUserInfo(HIBYTE(HIWORD(*pVer)), LOBYTE(HIWORD(*pVer)), HIBYTE(LOWORD(*pVer))));
	Send(OscarPackets::setIcbmParams());
	// Subscribe to presence for SSI contacts. The server sends 03/0B and
	// 03/0C updates to clients registered via BuddyAddBuddies (03/04).
	// Keep packets small to avoid exceeding server-side SNAC limits.
	// Snapshot UINs under the roster lock, but never perform blocking
	// Netlib_Send while that lock is held. On Windows XP, synchronous
	// sends can pause roster/UI updates for the socket timeout period.
	std::vector<uint32_t> buddyUins;
	{
		mir_cslock lck(m_csRoster);
		for (auto &it : m_arUsers)
			if (it->uin)
				buddyUins.push_back(it->uin);
	}
	{
		OscarWriter buddies;
		int count = 0;
		for (uint32_t buddyUin : buddyUins) {
			CMStringA uin;
			uin.Format("%u", buddyUin);
			if (buddies.size() + uin.GetLength() + 1 > 3500 && count) {
				Send(OscarSnac(3, 4, 0, buddies.data()));
				buddies = OscarWriter();
				count = 0;
			}
			buddies.ascii8(uin.c_str());
			++count;
		}
		if (count)
			Send(OscarSnac(3, 4, 0, buddies.data()));
		debugLogA("presence subscriptions: %u roster users", (unsigned)buddyUins.size());
	}
	KicqXpTrace("FinishLogin subscriptions done");
	Send(OscarPackets::clientReady());
	// Activate SSI before publishing our initial presence.
	Send(OscarPackets::rosterActivate());
	// Send only a 4-byte status TLV. The legacy DC-info/away-text
	// extensions are not needed for Open OSCAR Server presence.
	Send(OscarPackets::setStatus(MirandaToIcqStatus(m_iStatus), 256));
	Send(OscarPackets::offlineMsgsRequest(atoi(m_szUin), m_seq));

	m_bOnline = true;
	if (getByte("XStatusId", 0)) SendXStatus();
	m_bProbeAnswered = true;
	m_lastProbe = time(0);
	KicqXpTrace("FinishLogin online");
	debugLogA("online");
	ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)iOldStatus, m_iStatus);
}

// 13/08 (add), 13/09 (update), 13/0A (delete) pushed by the server
void CKicqProto::HandleSsiItem(const bytes_t &data, bool bDeleted)
{
	OscarReader r(data);
	bytes_t rawName = r.bytes(r.u16());
	int groupId = r.u16();
	int itemId = r.u16();
	int type = r.u16();
	OscarTlvList tlvs(r.bytes(r.u16()));

	if (type == 0x14 && groupId == 0) {
        m_ownIconItemId = bDeleted ? 0 : uint16_t(itemId);
        setWord("BartOwnIconItemId", m_ownIconItemId);
        if (bDeleted) {
            m_ownIconServerHash.clear();
            delSetting("BartOwnAvatarHash");
            delSetting("BartOwnAvatarPath");
            ReportSelfAvatarChanged();
        } else if (const auto *icon = tlvs.find(0x00d5)) {
            const bytes_t &v = icon->data;
            if (v.size() == 18 && uint8_t(v[1]) == 16) {
                const bytes_t hash = v.substr(2, 16);
                m_ownIconServerHash = hash;
                const auto path = getMStringW("BartOwnAvatarPath");
                if (getMStringA("BartOwnAvatarHash") != bart::hexHash(hash).c_str() ||
                    path.IsEmpty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    delSetting("BartOwnAvatarPath");
                    ReportSelfAvatarChanged();
                    QueueBartAvatar(uint32_t(atoi(m_szUin)), 1, hash);
                }
            }
        }
        return;
    }
	if (type == 1 && groupId != 0) {
		mir_cslock lck(m_csRoster);
		auto *pGroup = FindGroup(groupId);
		if (bDeleted) {
			if (pGroup)
				m_arGroups.remove(pGroup);
			return;
		}
		if (pGroup == nullptr)
			m_arGroups.insert(pGroup = new KicqGroup());
		pGroup->id = groupId;
		pGroup->name = OscarText::textOrCp1251(rawName);
		pGroup->bNotInList = tlvs.has(0x6A);
		return;
	}

	uint32_t uin = strtoul(rawName.c_str(), nullptr, 10);
	if (type != 0 || uin == 0)
		return;

	if (bDeleted) {
		mir_cslock lck(m_csRoster);
		if (auto *pUser = FindUser(uin))
			if (pUser->ssiId == itemId)
				pUser->ssiId = pUser->groupId = 0;
		return;
	}

	ApplyRosterItem(uin, groupId, itemId, tlvs, false);
}

void CKicqProto::HandleSsiResult(const bytes_t &data, uint32_t reqId)
{
	int result = OscarReader(data).u16();
	HandleOwnIconSsiAck(result, reqId);
	debugLogA("SSI result %d for request %u", result, reqId);

	switch (reqId) {
	case REQ_ADD_CONTACT:
		if (m_pendAdd.uin == 0)
			break;

		if (result == 0 || result == 0x0E) {
			char szUin[20];
			_itoa(m_pendAdd.uin, szUin, 10);
			{
				mir_cslock lck(m_csRoster);
				auto *pUser = AddUser(m_pendAdd.uin, m_pendAdd.hContact);
				pUser->groupId = m_pendAdd.groupId;
				pUser->ssiId = m_pendAdd.ssiId;
				pUser->bAuthorized = (result == 0);
			}
			setWString(m_pendAdd.hContact, "SrvNick", m_pendAdd.name);

			if (result == 0x0E) {
				// the contact requires authorization: add it flagged, then ask
				setByte(m_pendAdd.hContact, "Auth", 1);
				Send(OscarPackets::ssiEditStart());
				Send(OscarPackets::addContact(szUin, m_pendAdd.name, m_pendAdd.groupId, m_pendAdd.ssiId, true));
				Send(OscarPackets::ssiEditEnd());
				Send(OscarPackets::futureAuthGrant(szUin));
				Send(OscarPackets::authRequest(szUin, TranslateT("Please authorize me")));
			}
			m_pendAdd.uin = 0;
		}
		else {
			debugLogA("adding %u failed, code %d", m_pendAdd.uin, result);
			m_pendAdd.uin = 0;
		}
		break;

	case REQ_DEL_CONTACT:
		m_pendDel.uin = 0;
		break;

	case REQ_RENAME:
		if (result == 0 && m_pendRename.uin)
			if (MCONTACT hContact = FindContact(m_pendRename.uin))
				setWString(hContact, "SrvNick", m_pendRename.name);
		m_pendRename.uin = 0;
		break;

	case REQ_ADD_GROUP:
		if (result == 0 && !m_pendGroup.name.IsEmpty()) {
			{
				mir_cslock lck(m_csRoster);
				auto *pGroup = new KicqGroup();
				pGroup->id = m_pendGroup.groupId;
				pGroup->name = m_pendGroup.name;
				pGroup->bNotInList = false;
				m_arGroups.insert(pGroup);
			}

			if (m_hAddAfterGroup && m_wszAddAfterGroup == m_pendGroup.name)
				SsiAddContact(m_hAddAfterGroup, m_pendGroup.groupId);
		}
		m_hAddAfterGroup = 0;
		m_wszAddAfterGroup.Empty();
		m_pendGroup.name.Empty();
		break;
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// presence

// QIP Infium X-status GUIDs, read from the original catalog embedded in the project.
// The OSCAR 0x0003/0x000B buddy-online SNAC exposes these as 16-byte capabilities.
// This decoder is intentionally passive: it never changes ordinary presence or messaging.
static int FindQipXStatus(const bytes_t &caps)
{
	static const char *const guids[] = {
		"01D8D7EEAC3B492AA58DD3D877E66B92",
		"5A581EA1E580430CA06F612298B7E4C7",
		"83C9B78E77E74378B2C5FB6CFCC35BEC",
		"E601E41C33734BD1BC06811D6C323D81",
		"8C50DBAE81ED4786ACCA16CC3213C7B7",
		"3FB0BD36AF3B4A609EEFCF190F6A5A7F",
		"F8E8D7B282C4414290F810C6CE0A89A6",
		"80537DE2A4674A76B3546DFD075F5EC6",
		"F18AB52EDC57491D99DC6444502457AF",
		"1B78AE31FA0B4D3893D1997EEEAFB218",
		"61BEE0DD8BDD475D8DEE5F4BAACF19A7",
		"488E14898ACA4A0882AA77CE7A165208",
		"107A9A1812324DA4B6CD0879DB780F09",
		"6F4930984F7C4AFFA27634A03BCEAEA7",
		"1292E5501B644F66B206B29AF378E48D",
		"D4A611D08F014EC09223C5B6BEC6CCF0",
		"609D52F8A29A49A6B2A02524C5E9D260",
		"63627337A03F49FF80E5F709CDE0A4EE",
		"1F7A4071BF3B4E60BC324C5787B04CF1",
		"785E8C4840D34C65886F04CF3F3F43DF",
		"A6ED557E6BF744D4A5D4D2E7D95CE81F",
		"12D07E3EF885489E8E97A72A6551E58D",
		"BA74DB3E9E24434B87B62F6B8DFEE50F",
		"634F6BD8ADD24AA1AAB9115BC26D05A1",
		"2CE0E4E57C6443709C3A7A1CE878A7DC",
		"101117C9A3B040F981AC49E159FBD5D4",
		"160C60BBDD4443F39140050F00E6C009",
		"6443C6AF22604517B58CD7DF8E290352",
		"16F5B76FA9D240358CC5C084703C98FA",
		"631436FF3F8A40D0A5CB7B66E051B364",
		"B70867F538254327A1FFCF4CC1939797",
		"DDCF0EA971954048A9C6413206D6F280",
		"D4E2B0BA334E4FA598D0117DBF4D3CC8",
		"CD5643A2C94C4724B52CDC0124A1D0CD",
		"0072D9084AD143DD91996F026966026F",
	};
	OscarReader r(caps);
	while (r.remaining() >= 16) {
		CMStringA guid(OscarText::hex(r.bytes(16)));
		for (int i = 0; i < int(_countof(guids)); ++i)
			if (!mir_strcmpi(guid.c_str(), guids[i]))
				return i + 1;
	}
	return 0;
}

void CKicqProto::HandleUserOnline(const bytes_t &data)
{
	OscarReader r(data);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);
	r.skip(2); // warning level
	r.skip(2); // TLV count
	OscarTlvList tlvs(r);
	if (const auto *bartId = tlvs.find(0x1D)) ObserveBartPresence(uin, bartId->data);

    // Only trace changes for the target Jimm Multi UIN. A presence refresh can
    // repeat hundreds of times; logging each repeat hid the relevant packets.
    // This is deliberately diagnostic: TLV 0x001D can contain an X-status GUID
    // without containing any human-readable status message.
    if (uin == 79229) {
        const OscarTlv *note = tlvs.find(0x1D);
        const OscarTlv *away = tlvs.find(0x02);
        const OscarTlv *state = tlvs.find(0x06);
        CMStringA signature = note ? OscarText::hex(note->data) : CMStringA("-");
        signature += ":";
        signature += away ? OscarText::hex(away->data) : CMStringA("-");
        signature += ":";
        signature += state ? OscarText::hex(state->data) : CMStringA("-");
        static CMStringA lastSignature;
        if (signature != lastSignature) {
            lastSignature = signature;
            char line[256];
            mir_snprintf(line, "PRESENCE CHANGED: uin=%u, tlv001D=%u, tlv0002=%u, tlv0006=%u",
                (unsigned)uin, note ? (unsigned)note->data.size() : 0,
                away ? (unsigned)away->data.size() : 0,
                state ? (unsigned)state->data.size() : 0);
            KicqWriteXtrazDebug(line);
            if (note) {
                const unsigned limit = min((unsigned)note->data.size(), 512u);
                for (unsigned offset = 0; offset < limit; offset += 64) {
                    mir_snprintf(line, "PRESENCE 001D uin=79229 +%04u: %s", offset,
                        OscarText::hex(note->data.substr(offset, min(64u, limit - offset))).c_str());
                    KicqWriteXtrazDebug(line);
                }
            }
            if (away && !away->data.empty()) {
                mir_snprintf(line, "PRESENCE 0002 uin=79229: %s",
                    OscarText::hex(away->data.substr(0, min((unsigned)away->data.size(), 64u))).c_str());
                KicqWriteXtrazDebug(line);
            }
        }
    }

	MCONTACT hContact;
	bool bFull;
	bool xstatusChanged = false;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(uin);
		if (pUser == nullptr)
			return;

		hContact = pUser->hContact;
		pUser->bOnline = true;

		// an abbreviated status update carries no capabilities and must not wipe them
		auto *caps = tlvs.find(0x0D), *shortCaps = tlvs.find(0x19);
		bFull = caps || shortCaps;
		if (bFull) {
			// A full capability update replaces the previously advertised X-status.
			// Do not clear it on abbreviated online updates.
			int xstatus = caps ? FindQipXStatus(caps->data) : 0;
			if (caps) xstatusChanged = (getByte(hContact, "XStatusId", 0) != xstatus || (xstatus && !getByte(hContact, "XStatusLive", 0)));
			if (caps && xstatus)
				{
    setByte(hContact, "XStatusId", (uint8_t)xstatus);
    setByte(hContact, "XStatusLive", 1);
    // A capability GUID provides the status kind, not the remote custom title/text.
    // Keep a readable default name until Xtraz text exchange is implemented.
    if (xstatusChanged) setWString(hContact, "XStatusName", kKicqXStatuses[xstatus - 1].nameRu);
    if (xstatusChanged) {
     db_unset(hContact, m_szModuleName, "XStatusMsg");
     db_unset(hContact, m_szModuleName, "XStatusTextReceived");
     // An X-status capability only identifies the icon, never the user's text.
     // Do not invent a CList/StatusMsg from the catalogue (e.g. "Китай 3").
     // Keep a real OSCAR away message if the remote client has supplied one.
     // Keep the last genuine status text until a replacement arrives.
    }
   }
			else if (caps)
				{ db_unset(hContact, m_szModuleName, "XStatusId"); db_unset(hContact, m_szModuleName, "XStatusName"); db_unset(hContact, m_szModuleName, "XStatusLive"); db_unset(hContact, m_szModuleName, "XStatusMsg"); db_unset(hContact, m_szModuleName, "XStatusTextReceived"); db_unset(hContact, "CList", "StatusMsg"); db_unset(hContact, m_szModuleName, "XStatusQueryTS"); }
			/* Refresh after roster lock is released below. */
			pUser->bUtf8 = pUser->bServerRelay = false;
			CMStringA szMirVer;
			if (caps) {
				OscarReader cr(caps->data);
				while (cr.remaining() >= 16) {
					bytes_t raw = cr.bytes(16);
					CMStringA cap(OscarText::hex(raw));
					if (cap == CAP_UTF8)
						pUser->bUtf8 = true;
					else if (cap == CAP_SERVER_RELAY)
						pUser->bServerRelay = true;
					else if (!memcmp(raw.data(), "Jasmine ver\xFF", 12))
						szMirVer.Format("Jasmine IM %d.%d.%d", (uint8_t)raw[12], (uint8_t)raw[13], (uint8_t)raw[14]);
					else if (!memcmp(raw.data(), "Miranda PMLICQ", 14) && szMirVer.IsEmpty())
						szMirVer = "Miranda NG PMLICQ";
					else if (!memcmp(raw.data(), "Miranda KICQ", 12) && szMirVer.IsEmpty())
						szMirVer = "Miranda NG KICQ";
				}
			}
			if (shortCaps) {
				OscarReader cr(shortCaps->data);
				while (cr.remaining() >= 2) {
					int sc = cr.u16();
					if (sc == 0x134E)
						pUser->bUtf8 = true;
					else if (sc == 0x1349)
						pUser->bServerRelay = true;
				}
			}
			if (!szMirVer.IsEmpty())
				setString(hContact, "MirVer", szMirVer);
		}
	}

	auto *st = tlvs.find(6);
	int iStatus = st ? OscarReader(st->data, 2).u16() : ICQ_STATUS_ONLINE;
	// Diagnostic counters: log every 100th presence notification, without UINs.
	static volatile LONG xpPresenceCount = 0;
	const LONG xpSeen = InterlockedIncrement(&xpPresenceCount);
	if ((xpSeen % 100) == 0) {
		char xpLine[96];
		mir_snprintf(xpLine, "XP presence notifications=%ld extra-icon refresh=disabled", (long)xpSeen);
		KicqXpTrace(xpLine);
	}
	// Avoid spamming the contact-list/ExtraIcons with unchanged presence on XP.
	const WORD newStatus = IcqToMirandaStatus(iStatus);
	const bool presenceChanged = getWord(hContact, "Status", ID_STATUS_OFFLINE) != newStatus;
	if (presenceChanged)
		setWord(hContact, "Status", newStatus);
	// Apply the registered extra icon after releasing the roster lock.
	// A short capability update must not clear an existing X-status.
	if (xstatusChanged || presenceChanged)
		KicqRefreshSingleXStatus(hContact);
    // QIP can change the X-status title/description without changing its GUID.
    // Re-query on subsequent full presence notifications, throttled per contact.
    // This does not trigger ExtraIcon_SetAll, preserving the no-lag fix.
    if (getByte(hContact, "XStatusLive", 0) && m_bOnline && bFull) {
        const uint32_t now = uint32_t(time(nullptr));
        const uint32_t last = getDword(hContact, "XStatusQueryTS", 0);
        if (xstatusChanged || last == 0 || now < last || now - last >= 60) {
            setDword(hContact, "XStatusQueryTS", now);
            OscarWriter requestCookie;
            requestCookie.u32(now).u32(uin);
            CMStringA target(FORMAT, "%u", uin);
            Send(OscarPackets::xtrazAwayQuery(requestCookie.data(), target.c_str(), m_szUin.c_str()));
            char queryLog[192];
            mir_snprintf(queryLog, "XTRAZ outgoing AwayStat query: target=%u, own=%s, cookie=%s (presence refresh)",
                (unsigned)uin, m_szUin.c_str(), OscarText::hex(requestCookie.data()).c_str());
            KicqWriteXtrazDebug(queryLog);
        }
    }

	if (auto *since = tlvs.find(3))
		setDword(hContact, "LogonTS", since->u32());

	CMStringW wszAway;
	if (auto *avail = tlvs.find(0x1D)) {
        // OSCAR TLV 001D uses: word item-id, byte flags, byte length,
        // then 'length' bytes. It is NOT a normal word-length TLV list.
        // For example, Jimm's 0002 04 04 00000000 has an empty note,
        // not a four-byte human-readable status.
        OscarReader nr(avail->data);
        while (nr.remaining() >= 4) {
            const unsigned item = nr.u16();
            const unsigned flags = nr.u8();
            const unsigned length = nr.u8();
            if (length > nr.remaining())
                break;
            bytes_t raw = nr.bytes(length);
            if (item != 2 || raw.empty())
                continue;
            // The common status-note form starts with a big-endian
            // two-byte text length, followed by UTF-8/legacy bytes.
            // Do not interpret the empty four-byte 00000000 as text.
            if (raw.size() >= 2) {
                OscarReader textReader(raw);
                const unsigned textLength = textReader.u16();
                if (textLength > 0 && textLength <= raw.size() - 2) {
                    wszAway = OscarText::textOrCp1251(textReader.bytes(textLength));
                    wszAway.Trim();
                }
            }
            (void)flags;
        }
	}
    if (!wszAway.IsEmpty()) {
        // Prefer a genuine remote status message over the fallback title,
        // but do not overwrite a complete, successfully decoded Xtraz text.
        if (!getByte(hContact, "XStatusLive", 0) ||
            db_get_b(hContact, m_szModuleName, "XStatusTextReceived", 0) == 0)
            db_set_ws(hContact, "CList", "StatusMsg", wszAway);
    }
    // An absent or empty note on a partial presence update is not a request
    // to clear a previously received genuine text (Xtraz or standard note).
}

void CKicqProto::HandleUserOffline(const bytes_t &data)
{
	OscarReader r(data);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);

    {
        char line[96];
        mir_snprintf(line, "PRESENCE 03/0C offline: uin=%u", (unsigned)uin);
        KicqWriteXtrazDebug(line);
    }

	MCONTACT hContact;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(uin);
		if (pUser == nullptr)
			return;
		pUser->bOnline = false;
		hContact = pUser->hContact;
		{ db_unset(hContact, m_szModuleName, "XStatusId"); db_unset(hContact, m_szModuleName, "XStatusName"); db_unset(hContact, m_szModuleName, "XStatusLive"); db_unset(hContact, m_szModuleName, "XStatusMsg"); db_unset(hContact, m_szModuleName, "XStatusTextReceived"); db_unset(hContact, m_szModuleName, "XStatusQueryTS"); }
	}

	if (getWord(hContact, "Status", ID_STATUS_OFFLINE) != ID_STATUS_OFFLINE)
		setWord(hContact, "Status", ID_STATUS_OFFLINE);
 KicqRefreshSingleXStatus(hContact);
	db_unset(hContact, "CList", "StatusMsg");
	CallService(MS_PROTO_CONTACTISTYPING, hContact, PROTOTYPE_CONTACTTYPING_OFF);
}

/////////////////////////////////////////////////////////////////////////////////////////
// messages

void CKicqProto::DeliverMessage(uint32_t uin, const CMStringW &text, time_t when)
{
	MCONTACT hContact = FindContact(uin);
	if (hContact == 0)
		hContact = CreateContact(uin, true);
	{
		mir_cslock lck(m_csRoster);
		AddUser(uin, hContact);
	}

	CallService(MS_PROTO_CONTACTISTYPING, hContact, PROTOTYPE_CONTACTTYPING_OFF);

	T2Utf szText(text);
	DB::EventInfo dbei;
	dbei.iTimestamp = when;
	dbei.pBlob = szText.get();
	ProtoChainRecvMsg(hContact, dbei);
}

void CKicqProto::HandleIncomingMessage(const bytes_t &data)
{
	OscarReader r(data);
	bytes_t cookie = r.bytes(8);
	int channel = r.u16();
	bytes_t sender = r.ascii8();
	r.skip(2);                           // warning level
	int infoCount = r.u16();
	OscarTlvList info(r, infoCount);     // sender's presence TLVs
	OscarTlvList msgTlvs(r);

	uint32_t uin = strtoul(sender.c_str(), nullptr, 10);
	if (uin == 0)
		return;

	CMStringW text;
	bool bHaveText = false, bOffline = false;
	time_t when = time(0);

	if (channel == 1) {
		auto *t2 = msgTlvs.find(2);
		if (!t2)
			return;

		OscarReader m(t2->data);
		while (m.remaining() >= 4) {
			int fragId = m.u8();
			m.u8();                          // fragment version
			int len = m.u16();
			if (fragId == 1 && len >= 4) {
				int charset = m.u16();
				m.u16();                      // sub-charset
				text = OscarText::cleanup(OscarText::decodeCharset(m.bytes(len - 4), charset));
				bHaveText = true;
			}
			else m.skip(len);
		}

		if (auto *ts = msgTlvs.find(0x16)) {
			when = ts->u32();
			bOffline = true;
		}
	}
	else if (channel == 2) {
		auto *t5 = msgTlvs.find(5);
		if (!t5)
			return;

		OscarReader m(t5->data);
		int msgType = m.u16();
		m.skip(8);                          // cookie again
		CMStringA guid(OscarText::hex(m.bytes(16)));
		// OFT2 file rendezvous: inspect envelope without touching Xtraz processing.
		if (guid == CAP_FILE) {
			// Validate the full rendezvous envelope before touching its nested TLVs.
			// Reject malformed / duplicate file-info TLVs (and cookie mismatches).
			oft2::Rendezvous validated;
			if (!oft2::parse_rendezvous(
				(const unsigned char *)t5->data.data(), t5->data.size(), validated) ||
				memcmp(validated.cookie.data(), cookie.data(), 8) != 0)
				return;
			char line[320];
			CMStringA cookieHex = OscarText::hex(cookie);
			mir_snprintf(line, "OFT2 rendezvous: uin=%u, type=%u, cookie=%s, tlvBytes=%u",
				uin, (unsigned)msgType, cookieHex.c_str(), (unsigned)m.remaining());
			KicqWriteXtrazDebug(line);
			OscarTlvList fileTlvs(m);
			const int tags[] = { 0x0002, 0x0003, 0x0004, 0x0005, 0x000A, 0x000C, 0x2711, 0x2712 };
			for (int tag : tags) {
				if (auto *item = fileTlvs.find(tag)) {
					mir_snprintf(line, "OFT2 rendezvous TLV: uin=%u, tag=%04X, bytes=%u",
						uin, tag, (unsigned)item->data.size());
					KicqWriteXtrazDebug(line);
				}
			}
			if (auto *info = fileTlvs.find(0x2711)) {
				oft2::FileOffer offer;
				if (oft2::parse_file_info((const unsigned char *)info->data.data(), info->data.size(), offer)) {
					mir_snprintf(line, "OFT2 offer: uin=%u, files=%u, bytes=%u, name=%s",
						uin, (unsigned)offer.count, (unsigned)offer.size, offer.name.c_str());
					KicqWriteXtrazDebug(line);
				}
			}
			if (validated.type == 0 && validated.hasOffer) {
    MCONTACT hContact = FindContact(uin);
    if (!hContact) hContact = CreateContact(uin, true);
    HandleFileOffer(hContact, validated);
   }
   return;
		}
		if (msgType != 0)
			return;

		OscarTlvList inner(m);
		auto *ext = inner.find(0x2711);
		if (!ext)
			return;

		OscarReader e(ext->data);
		e.skip(e.u16le());                  // protocol/plugin block
		e.skip(e.u16le());                  // second block
		int type = e.u8();
		e.u8();                              // flags
		e.u16le();                           // status
		e.u16le();                           // priority
		int len = e.u16le();

		// Xtraz diagnostics: log the exact ICBM channel-2 plugin payload before
		// returning on the legacy message-type filter. Do not alter message delivery.
		// Only 0x1A is logged, keeping ordinary chats and passwords out of the log.
		if (type == 0x1A) {
			const bytes_t &payload = ext->data;
			char line[512];
			mir_snprintf(line, "XTRAZ incoming: sender=%s, cookie=%s, rendezvousGuid=%s, payloadSize=%u, pluginType=0x%02X",
				sender.c_str(), OscarText::hex(cookie).c_str(), guid.c_str(),
				(unsigned)payload.size(), type);
			KicqWriteXtrazDebug(line);
			// Bounded hexdump; no guesses about binary offsets or XML encoding.
			const size_t limit = min(payload.size(), size_t(2048));
			for (size_t offset = 0; offset < limit; offset += 64) {
				const bytes_t block = payload.substr(offset, min(size_t(64), limit - offset));
				mir_snprintf(line, "XTRAZ payload +%04u: %s", (unsigned)offset,
					OscarText::hex(block).c_str());
				KicqWriteXtrazDebug(line);
			}
			if (payload.size() > limit) {
				mir_snprintf(line, "XTRAZ payload truncated: %u bytes omitted", (unsigned)(payload.size() - limit));
				KicqWriteXtrazDebug(line);
			}
		}
		if (type == 0x1A) {
            // QIP/ICQ 6 sends a Script Plug-in AwayStat query in TLV 0x2711.
            // Its nested XML is entity-escaped (&lt;...&gt;).
            const bytes_t &payload = ext->data;
            const bytes_t pluginGuid = OscarText::unhex("3B60B3EFD82A6C45A4E09C5A5E67E865");
            // Jasmine and other OSCAR clients may use a different Xtraz
            // envelope or escape the outer <N> node.  Identify the AwayStat
            // request by its actual plugin GUID and service operation instead
            // of requiring the QIP-specific outer XML spelling.
            bytes_t decodedRequest = KicqUnescapeXml(payload);
            decodedRequest = KicqUnescapeXml(decodedRequest);
            if (payload.find(pluginGuid) != bytes_t::npos &&
                decodedRequest.find("AwayStat") != bytes_t::npos &&
                (decodedRequest.find("cAwaySrv") != bytes_t::npos ||
                 decodedRequest.find("srvMng") != bytes_t::npos)) {
                int ownId = getByte("XStatusId", 0);
                if (ownId >= 1 && ownId <= 35 && m_bOnline) {
                    CMStringW title = getMStringW("XStatusName");
                    CMStringW description = getMStringW("XStatusMsg");
                    Send(OscarPackets::xtrazAwayReply(cookie, sender.c_str(), m_szUin.c_str(),
                        ownId, title.c_str(), description.c_str()));
                    KicqWriteXtrazDebug("XTRAZ AwayStat response sent (SNAC 4/0B)");
                }
                else KicqWriteXtrazDebug("XTRAZ AwayStat ignored (no active X-status)");
            }
            else KicqWriteXtrazDebug("XTRAZ 0x1A ignored (not an AwayStat query)");
            return;
        }
		if (type != 1)
			return;                          // only plain text (0x1A is an Xtraz plugin request)

		bytes_t raw = e.bytes(len > 0 ? len - 1 : 0);
		e.skip(1);

		// fg/bg colours, then an optional encoding GUID
		bool bUtf8 = false;
		if (e.remaining() >= 8) {
			e.u32(); e.u32();
			if (e.remaining() >= 4) {
				bytes_t g = e.bytes(e.u32le());
				if (strstr(g.c_str(), "0946134E") || strstr(g.c_str(), "0946134e"))
					bUtf8 = true;
			}
		}

		if (bUtf8 && OscarText::isUtf8(raw))
			text = OscarText::fromUtf8(raw);
		else
			text = OscarText::decodeGuess(raw);
		text = OscarText::cleanup(text);
		bHaveText = true;
	}
	else return;

	if (!bHaveText)
		return;

	if (!bOffline)
		Send(OscarPackets::messageAck(cookie, sender.c_str()));
	DeliverMessage(uin, text, when);
}

void CKicqProto::HandleOfflineMessage(const bytes_t &data)
{
	OscarReader r(data);
	r.skip(10);
	int type = r.u16();
	if (type == 0x4100) {
		r.skip(2);
		uint32_t uin = r.u32le();
		int year = r.u16le();
		int month = r.u8();
		int day = r.u8();
		r.u8(); r.u8();                     // hour, minute
		r.skip(2);
		bytes_t raw = r.bytes(r.u16le());
		CMStringW text(OscarText::cleanup(OscarText::decodeGuess(raw)));

		CMStringW key(FORMAT, L"%u|%s", uin, text.c_str());
		{
			mir_cslock lck(m_csRoster);
			if (m_offlineSeen.count(key))
				return;
			m_offlineSeen.insert(key);
		}

		// the server's timestamp is in some private timezone (off by hours from UTC): the
		// date is kept, the time of day is not trusted
		time_t when = time(0);
		struct tm tmNow;
		localtime_s(&tmNow, &when);
		if (year > 1990 && (year < tmNow.tm_year + 1900 || (year == tmNow.tm_year + 1900 &&
			(month < tmNow.tm_mon + 1 || (month == tmNow.tm_mon + 1 && day < tmNow.tm_mday))))) {
			struct tm tmMsg = {};
			tmMsg.tm_year = year - 1900;
			tmMsg.tm_mon = month - 1;
			tmMsg.tm_mday = day;
			tmMsg.tm_hour = 23;
			tmMsg.tm_min = 59;
			tmMsg.tm_isdst = -1;
			when = mktime(&tmMsg);
		}
		DeliverMessage(uin, text, when);
	}
	else if (type == 0x4200) {
		Send(OscarPackets::deleteOfflineMsgs(atoi(m_szUin), m_seq));
		Send(OscarPackets::anotherOfflineMsgsRequest());
	}
}

// kicq.ru rewrites message cookies on the way back, so an ack is matched to the oldest
// message still pending for that uin

// Extract fields from the Xtraz response. The outer RES may be XML-escaped.
static bytes_t KicqUnescapeXml(bytes_t text)
{
 const struct { const char *from, *to; } replacements[] = {
  {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
  {"&apos;", "'"}, {"&amp;", "&"}
 };
 for (auto &r : replacements) {
  size_t pos = 0;
  while ((pos = text.find(r.from, pos)) != bytes_t::npos) {
   text.replace(pos, strlen(r.from), r.to);
   pos += strlen(r.to);
  }
 }
 return text;
}

static bytes_t KicqXmlField(const bytes_t &text, const char *name)
{
 // Jasmine may wrap values in XML tags with attributes (e.g. <title lang="ru">)
 // and may use mixed-case element names. Never scan past the closing tag.
 bytes_t lower = text;
 for (char &ch : lower)
  if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
 bytes_t field = name;
 for (char &ch : field)
  if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
 const bytes_t prefix = "<" + field;
 size_t start = 0;
 while ((start = lower.find(prefix, start)) != bytes_t::npos) {
  const size_t after = start + prefix.size();
  if (after >= lower.size() || (lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t')) {
   start = after;
   continue;
  }
  const size_t tagEnd = lower.find('>', after);
  if (tagEnd == bytes_t::npos) return {};
  const bytes_t close = "</" + field + ">";
  const size_t end = lower.find(close, tagEnd + 1);
  if (end == bytes_t::npos) return {};
  return text.substr(tagEnd + 1, end - tagEnd - 1);
 }
 return {};
}

void CKicqProto::HandleMessageAck(const bytes_t &data)
{
	OscarReader r(data);
	r.skip(8);                              // cookie
	r.u16();                                // channel
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);

	// A channel-2 message ACK may carry an Xtraz AwayStat response.
 // Never turn it into an ordinary chat message or acknowledge a pending chat.
 const bytes_t payload = r.rest();
 // Trace every channel-2 ACK, including non-XML responses, to distinguish
 // Jimm Multi from unrelated contacts. Bounded output, no chat logging.
 {
  char ackLine[192];
  mir_snprintf(ackLine, "XTRAZ ACK received: uin=%u, payload=%u, xmlCandidate=%u",
   (unsigned)uin, (unsigned)payload.size(),
   (unsigned)(payload.find("CASXtraSetAwayMessage") != bytes_t::npos ||
              payload.find("cAwaySrv") != bytes_t::npos));
  KicqWriteXtrazDebug(ackLine);
 }
 bytes_t xml = payload;
 // Different clients escape the XML response a different number of times.
 // Limit passes to avoid pathological packets and keep QIP/ICQ6 behaviour.
 for (int pass = 0; pass < 4; ++pass) {
  bytes_t next = KicqUnescapeXml(xml);
  if (next == xml) break;
  xml.swap(next);
 }
 if (xml.find("CASXtraSetAwayMessage") != bytes_t::npos ||
     (xml.find("cAwaySrv") != bytes_t::npos &&
      (xml.find("<title") != bytes_t::npos || xml.find("<desc") != bytes_t::npos))) {
  const bytes_t title = KicqXmlField(xml, "title");
  const bytes_t desc = KicqXmlField(xml, "desc");
  if ((!title.empty() || !desc.empty()) && uin != 0) {
   if (MCONTACT hContact = FindContact(uin)) {
    if (getByte(hContact, "XStatusLive", 0)) {
     setWString(hContact, "XStatusName", OscarText::fromUtf8(KicqUnescapeXml(title)));
     setWString(hContact, "XStatusMsg", OscarText::fromUtf8(KicqUnescapeXml(desc)));
     setByte(hContact, "XStatusTextReceived", 1);
     const bytes_t decodedDesc = KicqUnescapeXml(desc);
     const bytes_t decodedTitle = KicqUnescapeXml(title);
     // Tipper reads CList/StatusMsg. Show the complete remote X-status:
     // first line = title, second line = description (if present).
     // Keep the original XStatusName/XStatusMsg DB fields unchanged.
     bytes_t display = decodedTitle;
     if (!decodedDesc.empty()) {
      if (!display.empty()) display += " ";
      display += decodedDesc;
     }
     if (!display.empty())
      db_set_ws(hContact, "CList", "StatusMsg", OscarText::fromUtf8(display));
     // Empty decoded values must not erase a valid OSCAR status note.
     // Tipper can display these fields via its custom DB-field settings.
     char savedLine[192];
     mir_snprintf(savedLine, "XTRAZ remote AwayStat saved: uin=%u, titleBytes=%u, descBytes=%u",
      (unsigned)uin, (unsigned)title.size(), (unsigned)desc.size());
     KicqWriteXtrazDebug(savedLine);
    }
   }
  }
  else {
   char line[192];
   mir_snprintf(line, "XTRAZ reply title/desc missing: uin=%u, payload=%u, title=%u, desc=%u",
    (unsigned)uin, (unsigned)payload.size(), (unsigned)title.size(), (unsigned)desc.size());
   KicqWriteXtrazDebug(line);
   // Log the actual response bytes for clients such as Jimm Multi. A binary
   // rendezvous envelope can contain XML after non-text headers; without
   // this dump an unsupported response is indistinguishable from empty text.
   // Cap output to 768 bytes to avoid unbounded log growth.
   const size_t dumpSize = payload.size() < 768 ? payload.size() : 768;
   for (size_t offset = 0; offset < dumpSize; offset += 48) {
    char dumpLine[160];
    const size_t count = (dumpSize - offset < 48) ? dumpSize - offset : 48;
    mir_snprintf(dumpLine, "XTRAZ undecoded reply uin=%u +%04u: ",
     (unsigned)uin, (unsigned)offset);
    size_t used = strlen(dumpLine);
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < count && used + 2 < sizeof(dumpLine); ++i) {
     const unsigned char byte = (unsigned char)payload[offset + i];
     dumpLine[used++] = digits[byte >> 4];
     dumpLine[used++] = digits[byte & 15];
    }
    dumpLine[used] = 0;
    KicqWriteXtrazDebug(dumpLine);
   }
       // Never replace a missing Jimm Multi text with the catalogue's icon title.
    // The undecoded response dump above is needed to implement its actual format.
// Do not clear an existing status on malformed/empty replies.
  }
  return;
 }
 // The ACK was not recognized as a supported AwayStat XML response.
 // Record a bounded HEX dump so the exact Jimm Multi response can be decoded.
 if (uin != 0 && !payload.empty()) {
  char line[192];
  mir_snprintf(line, "XTRAZ unrecognized ACK: uin=%u, payload=%u", (unsigned)uin, (unsigned)payload.size());
  KicqWriteXtrazDebug(line);
  const size_t limit = payload.size() < 768 ? payload.size() : 768;
  for (size_t offset = 0; offset < limit; offset += 48) {
   const size_t count = (limit - offset < 48) ? limit - offset : 48;
   mir_snprintf(line, "XTRAZ unrecognized ACK uin=%u +%04u: %s",
    (unsigned)uin, (unsigned)offset, OscarText::hex(payload.substr(offset, count)).c_str());
   KicqWriteXtrazDebug(line);
  }
 }

	int seq = 0;
	{
		mir_cslock lck(m_csPending);
		for (auto it = m_pendingMsgs.begin(); it != m_pendingMsgs.end(); ++it) {
			if (it->second.uin == uin) {
				seq = it->second.seq;
				m_pendingMsgs.erase(it);
				break;
			}
		}
	}

	if (seq)
		if (MCONTACT hContact = FindContact(uin))
			ProtoBroadcastAck(hContact, ACKTYPE_MESSAGE, ACKRESULT_SUCCESS, (HANDLE)seq, 0);
}

// errors answer the request id they refer to: our keep-alive probe (error 9, by design),
// an ack we sent for a stranger's message (error 14), or a message of ours
void CKicqProto::HandleMessageError(const bytes_t &data, uint32_t reqId)
{
	int code = OscarReader(data).u16();

	KicqPendingMsg msg;
	{
		mir_cslock lck(m_csPending);
		auto it = m_pendingMsgs.find(reqId);
		if (it == m_pendingMsgs.end())
			return;
		msg = it->second;
		m_pendingMsgs.erase(it);
	}

	debugLogA("message to %u failed, error %d", msg.uin, code);
	if (MCONTACT hContact = FindContact(msg.uin)) {
		CMStringW wszError(FORMAT, TranslateT("The server refused the message (error %d)"), code);
		ProtoBroadcastAck(hContact, ACKTYPE_MESSAGE, ACKRESULT_FAILED, (HANDLE)msg.seq, (LPARAM)wszError.c_str());
	}
}

void CKicqProto::HandleTyping(const bytes_t &data)
{
	OscarReader r(data);
	r.skip(10);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);
	int type = r.u16();

	if (MCONTACT hContact = FindContact(uin))
		CallService(MS_PROTO_CONTACTISTYPING, hContact, type == 2 ? PROTOTYPE_CONTACTTYPING_INFINITE : PROTOTYPE_CONTACTTYPING_OFF);
}

/////////////////////////////////////////////////////////////////////////////////////////
// user details: the meta "full user info" reply comes in chunks; 0xFA is the last one

static CMStringW MetaString(OscarReader &r)
{
	bytes_t raw = r.bytes(r.u16le());
	CMStringW s(OscarText::decodeGuess(raw));
	s.Trim();
	return s;
}

void CKicqProto::HandleUserInfo(const bytes_t &data, uint32_t reqId)
{
	KicqInfoRequest req;
	{
		mir_cslock lck(m_csPending);
		auto it = m_infoRequests.find(reqId);
		if (it == m_infoRequests.end())
			return;
		req = it->second;
	}

	OscarReader r(data);
	r.skip(14);                             // TLV head, chunk length, our UIN, 0x07DA, sequence
	int chunk = r.u16le();
	int result = r.u8();

	MCONTACT hContact = req.hContact;
	auto setStr = [this, hContact, &req](const char *szSetting, const CMStringW &val)
	{
		// Search results aren't an account profile. For own GetInfo, the
		// database record lives at contact 0 (account settings).
		if (hContact == 0 && req.hSearch) return;
		// Some OSCAR servers return empty fields for information they do not
		// expose. Do not erase an existing profile with an empty response.
		if (val.IsEmpty()) return;
		else {
			if (hContact) setWString(hContact, szSetting, val);
			else setWString(szSetting, val);
		}
	};

	// Ignore database-change callbacks while applying the server's own profile.
	const bool updatingOwnProfile = (hContact == 0 && !req.hSearch);
	if (updatingOwnProfile) m_bProfileSync = true;
	bool bDone = (result != 0x0A);          // the UIN is unknown (or the request was refused)
	if (!bDone) {
		switch (chunk) {
		case 0xC8:                           // basic
			req.nick = MetaString(r);
			req.firstName = MetaString(r);
			req.lastName = MetaString(r);
			req.email = MetaString(r);
			setStr("Nick", req.nick);
			setStr("FirstName", req.firstName);
			setStr("LastName", req.lastName);
			setStr("e-mail", req.email);
			setStr("City", MetaString(r));
			setStr("State", MetaString(r));
			setStr("Phone", MetaString(r));
			setStr("Fax", MetaString(r));
			setStr("Street", MetaString(r));
			setStr("Cellular", MetaString(r));
			setStr("ZIP", MetaString(r));
			if (hContact) setWord(hContact, "Country", r.u16le());
			break;

		case 0xDC:                           // more
			{
				int age = r.u16le();
				int gender = r.u8();
				CMStringW homepage(MetaString(r));
				int year = r.u16le();
				int month = r.u8();
				int day = r.u8();
				if (age) { if (hContact) setWord(hContact, "Age", age); else setWord("Age", age); }
				if (gender == 1) { if (hContact) setByte(hContact, "Gender", 'F'); else setByte("Gender", 'F'); }
				else if (gender == 2) { if (hContact) setByte(hContact, "Gender", 'M'); else setByte("Gender", 'M'); }
				setStr("Homepage", homepage);
				if (year && month && day) {
					if (hContact) { setWord(hContact, "BirthYear", year); setByte(hContact, "BirthMonth", month); setByte(hContact, "BirthDay", day); }
                    else { setWString("BirthYear", std::to_wstring(year).c_str()); setWString("BirthMonth", std::to_wstring(month).c_str()); setWString("BirthDay", std::to_wstring(day).c_str()); }
				}
			}
			break;

		case 0xD2:                           // work
			for (int i = 0; i < 6; i++)
				MetaString(r);                 // city, state, phone, fax, address, zip
			r.u16le();                        // country
			setStr("Company", MetaString(r));
			setStr("CompanyDepartment", MetaString(r));
			setStr("CompanyPosition", MetaString(r));
			break;

		case 0xE6:                           // about
			setStr("About", MetaString(r));
			break;

		case 0xFA:                           // affiliations
			bDone = true;
			break;
		}
	}

	if (updatingOwnProfile) m_bProfileSync = false;
	{
		mir_cslock lck(m_csPending);
		if (bDone)
			m_infoRequests.erase(reqId);
		else
			m_infoRequests[reqId] = req;
	}

	if (bDone) {
		if (req.hSearch) {
			CMStringW wszUin(FORMAT, L"%u", req.uin);
			PROTOSEARCHRESULT psr = {};
			psr.cbSize = sizeof(psr);
			psr.flags = PSR_UNICODE;
			psr.id.w = wszUin.GetBuffer();
			psr.nick.w = req.nick.GetBuffer();
			psr.firstName.w = req.firstName.GetBuffer();
			psr.lastName.w = req.lastName.GetBuffer();
			psr.email.w = req.email.GetBuffer();
			ProtoBroadcastAck(0, ACKTYPE_SEARCH, ACKRESULT_DATA, req.hSearch, (LPARAM)&psr);
			ProtoBroadcastAck(0, ACKTYPE_SEARCH, ACKRESULT_SUCCESS, req.hSearch, 0);
		}
		else ProtoBroadcastAck(hContact, ACKTYPE_GETINFO, ACKRESULT_SUCCESS, (HANDLE)1, 0);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// authorization

void CKicqProto::HandleAuthRequest(const bytes_t &data)
{
	OscarReader r(data);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);
	CMStringW reason(OscarText::textOrCp1251(r.bytes(r.u16())));
	if (uin == 0)
		return;

	MCONTACT hContact = FindContact(uin);
	if (hContact == 0)
		hContact = CreateContact(uin, true);

	char szUin[20];
	_itoa(uin, szUin, 10);
	DB::AUTH_BLOB blob(hContact, T2Utf(getMStringW(hContact, "Nick")), nullptr, nullptr, szUin, T2Utf(reason));
	blob.set_uin(uin);

	DB::EventInfo dbei;
	dbei.iTimestamp = time(0);
	dbei.cbBlob = blob.size();
	dbei.pBlob = blob;
	ProtoChainRecv(hContact, PSR_AUTH, 0, (LPARAM)&dbei);
}

void CKicqProto::HandleAuthReply(const bytes_t &data)
{
	OscarReader r(data);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);
	bool bGranted = r.u8() == 1;
	debugLogA("authorization %s by %u", bGranted ? "granted" : "denied", uin);

	MCONTACT hContact = FindContact(uin);
	if (hContact == 0 || !bGranted)
		return;

	delSetting(hContact, "Auth");

	mir_cslock lck(m_csRoster);
	if (auto *pUser = FindUser(uin))
		pUser->bAuthorized = true;
}

void CKicqProto::HandleYouWereAdded(const bytes_t &data)
{
	OscarReader r(data);
	uint32_t uin = strtoul(r.ascii8().c_str(), nullptr, 10);
	if (uin == 0)
		return;

	MCONTACT hContact = FindContact(uin);
	if (hContact == 0)
		hContact = CreateContact(uin, true);

	char szUin[20];
	_itoa(uin, szUin, 10);
	DB::AUTH_BLOB blob(hContact, T2Utf(getMStringW(hContact, "Nick")), nullptr, nullptr, szUin, nullptr);
	blob.set_uin(uin);

	DB::EventInfo dbei;
	dbei.szModule = m_szModuleName;
	dbei.iTimestamp = time(0);
	dbei.flags = DBEF_UTF;
	dbei.eventType = EVENTTYPE_ADDED;
	dbei.cbBlob = blob.size();
	dbei.pBlob = blob;
	db_event_add(0, &dbei);
}

// Secondary OSCAR service connection. It must not reuse or close the BOS socket.
DWORD WINAPI CKicqProto::BartThreadEntry(LPVOID arg)
{
 auto *self = static_cast<CKicqProto *>(arg);
 self->BartThread();
 return 0;
}

bool CKicqProto::BartSend(HNETLIBCONN connection, uint8_t channel,
 const bytes_t &payload, uint16_t &sequence)
{
 std::string frame;
 if (!bart::makeFlap({ channel, sequence++, payload }, frame)) return false;
 for (size_t offset = 0; offset < frame.size();) {
  int sent = Netlib_Send(connection, frame.data() + offset,
   (int)(frame.size() - offset), MSG_NODUMP);
  if (sent <= 0) return false;
  offset += sent;
 }
 return true;
}

// Requests stay queued until the BART connection can send them.
void CKicqProto::ObserveBartPresence(uint32_t uin, const bytes_t &raw)
{
 if (!uin || raw.empty()) return;
 MCONTACT contact = FindContact(uin);
 if (!contact) return;
 size_t offset = 0;
 while (offset < raw.size()) {
  bart::AssetId id;
  if (!bart::parseId(raw, offset, id)) {
   debugLogA("BART: invalid avatar metadata for %u", uin);
   return;
  }
  if (id.type != 1 || id.hash.empty() || id.hash.size() > 64) continue;
  const std::string hash = bart::hexHash(id.hash);
  // The hash in the database denotes a *downloaded* image, not merely an
  // advertised one. Failed requests must be eligible for another attempt.
  const auto stored = getMStringA(contact, "BartAvatarHash");
  const auto path = getMStringW(contact, "BartAvatarPath");
  if (stored == hash.c_str() && !path.IsEmpty() &&
      GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
  debugLogA("BART: contact %u advertises avatar type=%u hash=%s", uin, (unsigned)id.type, hash.c_str());
  QueueBartAvatar(uin, id.type, id.hash);
 }
}

void CKicqProto::QueueBartAvatar(uint32_t uin, uint16_t type, const bytes_t &hash)
{
 if (!uin || hash.empty() || hash.size() > 64) return;
 const std::string name = std::to_string(uin);
 bart::AssetId id; id.type = type; id.hash = hash;
 bytes_t payload;
 if (!bart::makeDownload(name, id, payload)) return;
 bart::SnacFrame request{0x10, 0x06, 0, 0, payload};
 bytes_t wire;
 if (!bart::makeSnac(request, wire)) return;
 mir_cslock lock(m_csBart);
 for (const auto &item : m_bartPending)
  if (item.screenName == name && item.hash == hash && item.type == type) return;
 m_bartPending.push_back({name, hash, type});
 m_bartOutgoing.push_back(wire);
 debugLogA("BART: queued image request for %u (type %u)", uin, (unsigned)type);
}

void CKicqProto::FlushBartRequests(HNETLIBCONN conn, uint16_t &seq)
{
 std::vector<bytes_t> ready;
 {
  mir_cslock lock(m_csBart);
  ready.swap(m_bartOutgoing);
 }
 for (size_t i = 0; i < ready.size(); ++i) {
  if (!BartSend(conn, 2, ready[i], seq)) {
   mir_cslock lock(m_csBart);
   m_bartOutgoing.insert(m_bartOutgoing.end(), ready.begin() + i, ready.end());
   return;
  }
 }
}

void CKicqProto::HandleOwnIconSsiAck(int result, uint32_t request)
{
 bytes_t upload;
 bool deleting = false;
 {
  mir_cslock lock(m_csBart);
  if (!m_ownIconSsiRequest || request != m_ownIconSsiRequest) return;
  m_ownIconSsiRequest = 0;
  deleting = m_ownIconDeleting;
  if (result == 0 && !deleting) upload = m_ownIconPendingUpload;
  if (result != 0) {
   m_ownIconPendingUpload.clear(); m_ownIconPendingHash.clear();
   m_ownIconPendingPath.Empty();
  }
 }
 if (result != 0) { debugLogA("BART SSI: server rejected own icon update: %d", result); return; }
 if (deleting) {
  m_ownIconItemId = 0;
  setWord("BartOwnIconItemId", 0);
  delSetting("BartOwnAvatarPath");
  delSetting("BartOwnAvatarHash");
  m_ownIconServerHash.clear();
  mir_cslock lock(m_csBart);
  m_ownIconPendingUpload.clear(); m_ownIconPendingHash.clear(); m_ownIconPendingPath.Empty();
  ReportSelfAvatarChanged();
  debugLogA("BART SSI: own avatar removed");
 } else {
  if (!m_ownIconItemId) m_ownIconItemId = 0x4000;
  setWord("BartOwnIconItemId", m_ownIconItemId);
  // Some servers send SNAC(1,21) to request upload, others accept upload directly.
  if (!upload.empty()) {
   bytes_t payload, wire;
   if (bart::makeUpload(1, upload, payload) && bart::makeSnac({0x10,0x02,0,0,payload}, wire)) {
    mir_cslock lock(m_csBart);
    m_bartOutgoing.push_back(wire);
   }
  }
 }
}

void CKicqProto::HandleBartUploadReply(const bytes_t &payload)
{
 // BART 10/03: result is ONE byte, followed by BART ID (type/flags/len/hash).
 // The old 16-bit result shifted all fields and discarded successful uploads.
 if (payload.size() < 5) return;
 size_t offset = 0; uint8_t result = 0;
 if (!bart::get8(payload, offset, result)) return;
 bart::AssetId id;
 if (result != 0 || !bart::parseId(payload, offset, id) || offset != payload.size()) {
  debugLogA("BART: server rejected upload (code %u)", unsigned(result)); return;
 }
 CMStringW path;
 bytes_t uploaded;
 {
  mir_cslock lock(m_csBart);
  if (id.hash != m_ownIconPendingHash || m_ownIconPendingPath.IsEmpty()) return;
  path = m_ownIconPendingPath;
  uploaded = m_ownIconPendingUpload;
  m_ownIconPendingUpload.clear(); m_ownIconPendingHash.clear(); m_ownIconPendingPath.Empty();
 }
 // The caller's temporary source file may disappear after SetMyAvatar returns.
 // Preserve the acknowledged bytes under our own per-account BART cache.
 wchar_t temp[MAX_PATH] = {};
 DWORD n = GetTempPathW(MAX_PATH, temp);
 if (n && n < MAX_PATH) {
  wchar_t folder[MAX_PATH] = {};
  if (swprintf_s(folder, L"%sPMLICQ-BART-%u", temp, (unsigned)getDword("UIN", 0)) > 0 &&
      (CreateDirectoryW(folder, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) {
   CMStringW cachePath;
   const char *extension = bart::imageExtension(uploaded);
   if (extension) {
    cachePath.Format(L"%s\\my-avatar%s", folder,
      !_stricmp(extension, ".png") ? L".png" :
      !_stricmp(extension, ".gif") ? L".gif" :
      !_stricmp(extension, ".bmp") ? L".bmp" : L".jpg");
    HANDLE handle = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
     DWORD written = 0;
     BOOL saved = WriteFile(handle, uploaded.data(), DWORD(uploaded.size()), &written, nullptr);
     CloseHandle(handle);
     if (saved && written == uploaded.size()) path = cachePath;
     else DeleteFileW(cachePath.c_str());
    }
   }
  }
 }
 setWString("BartOwnAvatarPath", path);
 setString("BartOwnAvatarHash", bart::hexHash(id.hash).c_str());
 m_ownIconServerHash = id.hash;
 ReportSelfAvatarChanged();
 debugLogA("BART: own avatar uploaded and acknowledged");
}

void CKicqProto::HandleBartSnac(const bytes_t &payload)
{
 bart::SnacFrame snac;
 if (!bart::parseSnac(payload, snac)) return;
 if (snac.family != 0x10) return;
 if (snac.subtype == 0x03) { HandleBartUploadReply(snac.payload); return; }
 if (snac.subtype != 0x07) {
  debugLogA("BART: SNAC(10,%02x)", (unsigned)snac.subtype);
  return;
 }
 bart::ImageReply reply;
 if (!bart::parseDownloadReply(snac.payload, reply)) {
  debugLogA("BART: malformed download response"); return;
 }
 // Never associate an unsolicited image with a contact.
 bool matched = false;
 {
  mir_cslock lock(m_csBart);
  for (auto it = m_bartPending.begin(); it != m_bartPending.end(); ++it) {
   if (it->screenName == reply.screenName && it->hash == reply.id.hash && it->type == reply.id.type) {
    m_bartPending.erase(it);
    matched = true;
    break;
   }
  }
 }
 if (!matched) {
  debugLogA("BART: unsolicited or unmatched image reply for %s", reply.screenName.c_str());
  return;
 }
 if (reply.result != 0 || !bart::isSupportedImage(reply.image)) {
  debugLogA("BART: image request failed for %s (result=%u size=%u)",
   reply.screenName.c_str(), (unsigned)reply.result, (unsigned)reply.image.size());
  return;
 }
 // Cache path is segregated by Miranda account and uses only a hexadecimal hash.
 wchar_t temp[MAX_PATH] = {};
 DWORD n = GetTempPathW(MAX_PATH, temp);
 if (!n || n >= MAX_PATH) return;
 wchar_t folder[MAX_PATH] = {};
 if (swprintf_s(folder, L"%sPMLICQ-BART-%u", temp, (unsigned)getDword("UIN", 0)) < 0) return;
 if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
 // Use the wide Windows API for writing: the system temp path may contain non-ASCII letters.
 const char *ext = bart::imageExtension(reply.image);
 const std::string name = bart::hexHash(reply.id.hash);
 if (!ext || name.empty()) return;
 std::wstring wideName(name.begin(), name.end());
 std::wstring wideExt(ext, ext + strlen(ext));
 std::wstring path = std::wstring(folder) + L"\\" + wideName + wideExt;
 HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
 if (file == INVALID_HANDLE_VALUE) return;
 DWORD written = 0;
 BOOL ok = WriteFile(file, reply.image.data(), DWORD(reply.image.size()), &written, nullptr);
 CloseHandle(file);
 if (!ok || written != reply.image.size()) { DeleteFileW(path.c_str()); return; }
 // When the requested UIN is ours, this is the previously published SSI avatar.
 // Save it separately so "Owner -> Avatar" can display it after a fresh login.
 if (reply.screenName == m_szUin.c_str() && reply.id.hash == m_ownIconServerHash) {
  setWString("BartOwnAvatarPath", path.c_str());
  setString("BartOwnAvatarHash", name.c_str());
  ReportSelfAvatarChanged();
  debugLogA("BART: restored own avatar from server SSI");
  return;
 }
 // Store per-contact cache location. The Avatar API must subsequently use this file.
 const unsigned long uin = strtoul(reply.screenName.c_str(), nullptr, 10);
 if (uin && std::to_string(uin) == reply.screenName) {
  MCONTACT contact = FindContact((uint32_t)uin);
  if (contact) {
   db_set_ws(contact, m_szModuleName, "BartAvatarPath", path.c_str());
   db_set_s(contact, m_szModuleName, "BartAvatarHash", name.c_str());
   db_set_ws(contact, "ContactPhoto", "File", path.c_str());
   PROTO_AVATAR_INFORMATION info = {};
   info.hContact = contact;
   if (!_stricmp(ext, ".png")) info.format = PA_FORMAT_PNG;
   else if (!_stricmp(ext, ".jpg")) info.format = PA_FORMAT_JPEG;
   else if (!_stricmp(ext, ".gif")) info.format = PA_FORMAT_GIF;
   else info.format = PA_FORMAT_BMP;
   wcscpy_s(info.filename, _countof(info.filename), path.c_str());
   // Synchronous ACK: Miranda AVS consumes the pointer during this call.
   // A nullptr on SUCCESS means 'no avatar' and caused the empty UI.
   ProtoBroadcastAck(contact, ACKTYPE_AVATAR, ACKRESULT_SUCCESS, (HANDLE)&info);
   debugLogA("BART: cached and announced contact %lu avatar", uin);
  }
 }
}

void CKicqProto::BartThread()
{
 // Snapshot redirect information before entering the service loop.
 CMStringA host;
 bytes_t cookie;
 int port;
 {
  mir_cslock lock(m_csBart);
  host = m_bartHost; cookie = m_bartCookie; port = m_bartPort;
 }
 if (host.IsEmpty() || cookie.empty()) return;
 HNETLIBCONN connection = Netlib_OpenConnection(m_hNetlibUser, host.c_str(), port, 20);
 if (!connection) { debugLogA("BART: TCP connection failed"); return; }
 {
  mir_cslock lock(m_csBart);
  m_hBartConn = connection;
 }
 uint16_t sequence = 1;
 bool bartReady = false;
 std::string auth;
 bool ok = bart::makeCookieLogin(cookie, sequence++, auth);
 if (ok) {
  for (size_t n = 0; n < auth.size();) {
   int sent = Netlib_Send(connection, auth.data() + n,
    (int)(auth.size() - n), MSG_NODUMP);
   if (sent <= 0) { ok = false; break; }
   n += sent;
  }
 }
 bytes_t inbound;
 char buffer[8192];
 while (ok && !m_bTerminated && !InterlockedCompareExchange(&m_bartStop, 0, 0)) {
  NETLIBSELECT selectInfo = {};
  selectInfo.dwTimeout = 1000;
  selectInfo.hReadConns[0] = connection;
  int ready = Netlib_Select(&selectInfo);
  if (ready < 0) break;
  if (!ready) { if (bartReady) FlushBartRequests(connection, sequence); continue; }
  int n = Netlib_Recv(connection, buffer, sizeof(buffer), MSG_NODUMP);
  if (n <= 0) break;
  inbound.append(buffer, n);
  bart::FlapFrame frame;
  int result;
  while ((result = bart::consumeFlap(inbound, frame)) == 1) {
   if (frame.channel == 1) {
    debugLogA("BART: authenticated service connection");
   }
   else if (frame.channel == 2) {
    bart::SnacFrame packet;
    if (!bart::parseSnac(frame.payload, packet)) { ok = false; break; }
    if (packet.family == 1 && (packet.subtype == 3 || packet.subtype == 0x15)) {
     // Advertised families: request BART version, but do not transmit
     // image requests until the service has acknowledged our version.
     bart::SnacFrame versionRequest{1, 0x17, 0, 0, std::string("\x00\x10\x00\x01", 4)};
     bytes_t snac;
     ok = bart::makeSnac(versionRequest, snac) && BartSend(connection, 2, snac, sequence);
     debugLogA("BART: requested family 0x10 version 1");
    }
    else if (packet.family == 1 && packet.subtype == 0x18 && !bartReady) {
     // OSCAR OSERVICE CLIENT_ONLINE (1/02): mandatory on the redirected
     // BART connection. Previous code skipped it and sent downloads too early.
     bytes_t clientInfo;
     bart::wire16(clientInfo, 0x0010); // BART family
     bart::wire16(clientInfo, 0x0001); // negotiated version
     bart::wire16(clientInfo, 0x0110); // tool ID
     bart::wire16(clientInfo, 0x164F); // tool version
     bytes_t snac;
     ok = bart::makeSnac({1, 0x02, 0, 0, clientInfo}, snac) &&
      BartSend(connection, 2, snac, sequence);
     if (ok) { bartReady = true; debugLogA("BART: client online sent, service ready"); }
    }
    else if (packet.family == 1 && packet.subtype == 0x01) {
     debugLogA("BART: service error (payload length=%u)", (unsigned)packet.payload.size());
    }
    HandleBartSnac(frame.payload);
   }
   else if (frame.channel == 4) { ok = false; break; }
  }
  if (result < 0) break;
  if (bartReady) FlushBartRequests(connection, sequence);
 }
 {
  mir_cslock lock(m_csBart);
  if (m_hBartConn == connection) m_hBartConn = nullptr;
 }
 Netlib_CloseHandle(connection);
 // Requests sent before a disconnect have no durable delivery guarantee.
 // Rebuild the queue from pending IDs for the next BART service connection,
 // instead of leaving them permanently stuck in m_bartPending.
 {
  mir_cslock lock(m_csBart);
  m_bartOutgoing.clear();
  for (const auto &pending : m_bartPending) {
   bart::AssetId id;
   id.type = pending.type;
   id.hash = pending.hash;
   bytes_t payload, packet;
   if (bart::makeDownload(pending.screenName, id, payload) &&
       bart::makeSnac({0x10, 0x06, 0, 0, payload}, packet))
    m_bartOutgoing.push_back(packet);
  }
  debugLogA("BART: disconnected; %u pending avatar requests retained for reconnect",
   (unsigned)m_bartPending.size());
 }
 debugLogA("BART: secondary connection closed");
 // Do not reuse a service cookie after disconnect. Request a new redirect
 // from BOS when a download is still pending. The BOS handler reaps this
 // finished worker and starts a new one on receipt of SNAC(1,5).
 if (!m_bTerminated && !InterlockedCompareExchange(&m_bartStop, 0, 0)) {
  bool pending = false;
  {
   mir_cslock lock(m_csBart);
   pending = !m_bartPending.empty();
  }
  if (pending) {
   bytes_t family; family.push_back(0); family.push_back(0x10);
   debugLogA("BART: requesting fresh redirect for pending avatar downloads");
   Send(OscarSnac(1, 4, 0, family));
  }
 }
}

void CKicqProto::StopBart()
{
 InterlockedExchange(&m_bartStop, 1);
 {
  mir_cslock lock(m_csBart);
  if (m_hBartConn) Netlib_Shutdown(m_hBartConn);
 }
 if (m_hBartThread) {
  WaitForSingleObject(m_hBartThread, INFINITE);
  CloseHandle(m_hBartThread);
  m_hBartThread = nullptr;
 }
}
