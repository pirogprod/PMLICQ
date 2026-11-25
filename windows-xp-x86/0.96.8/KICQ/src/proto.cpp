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

#include "stdafx.h"
#include "kicq_diag_trace.h"
#include "xstatus_catalog.h"
#include "oft2_socket_transfer.h"
#include "ars_proxy.h"
#include "oft2_rendezvous.h"
#include <process.h>
#include <algorithm>
#include <m_extraicons.h>
#include <m_genmenu.h>
#include <m_avatars.h>
#include "bart_codec.h"
#include "bart_wire.h"
#include "bart_ssi.h"

// Shared QIP sprite icons: one icon per X-status, loaded once per process.
static HICON g_kicqXIcons[35] = {};
static HANDLE g_kicqExtraIcon = nullptr;
static HANDLE g_kicqExtraImages[35] = {};

// One shared extra-icon registration serves every PMLICQ account.
// Never store one account name globally: a second account would hide the first.
static std::vector<CMStringA> g_kicqIconModules;
static bool KicqIsOurAccount(const char *module)
{
 if (!module) return false;
 for (const auto &name : g_kicqIconModules)
  if (!mir_strcmp(name.c_str(), module)) return true;
 return false;
}



static HICON KicqXIcon(int id)
{
 if (id < 1 || id > 35) return nullptr;
 HICON &cached = g_kicqXIcons[id - 1];
 if (cached) return cached;
 cached = (HICON)LoadImageW(g_plugin.getInst(), MAKEINTRESOURCEW(29100 + id - 1),
  IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
 return cached;
}

static int KicqExtraRebuild(WPARAM, LPARAM)
{
 for (int i = 1; i <= 35; ++i) {
  g_kicqExtraImages[i - 1] = INVALID_HANDLE_VALUE;
  if (HICON icon = KicqXIcon(i)) g_kicqExtraImages[i - 1] = ExtraIcon_AddIcon(icon);
 }
 return 0;
}
static int KicqExtraApply(WPARAM wParam, LPARAM)
{
 if (!g_kicqExtraIcon) return 0;
 MCONTACT contact = (MCONTACT)wParam;
 if (!contact) return 0;
 const char *module = Proto_GetBaseAccountName(contact);
 if (!KicqIsOurAccount(module)) return 0;
 // Local self-contact must read the account X-status, not remote XStatusLive.
 const uint32_t ownUin = db_get_dw(0, module, "UIN", 0);
 const uint32_t contactUin = db_get_dw(contact, module, "UIN", 0);
 const bool isSelf = ownUin != 0 && ownUin == contactUin;
 int id = 0;
 if (isSelf)
  id = db_get_b(0, module, "XStatusId", 0);
 else {
  const WORD presence = db_get_w(contact, module, "Status", ID_STATUS_OFFLINE);
  const bool confirmed = db_get_b(contact, module, "XStatusLive", 0) != 0;
  if (presence != ID_STATUS_OFFLINE && confirmed)
   id = db_get_b(contact, module, "XStatusId", 0);
 }
 if (id >= 1 && id <= 35) {
  // ExtraIcon_AddIcon may return image index 0 (the first registered icon).
  // Index 0 is valid; only INVALID_HANDLE_VALUE means no image.
  if (g_kicqExtraImages[id - 1] != INVALID_HANDLE_VALUE) ExtraIcon_SetIcon(g_kicqExtraIcon, contact, g_kicqExtraImages[id - 1]);
  else ExtraIcon_SetIcon(g_kicqExtraIcon, contact, INVALID_HANDLE_VALUE);
 }
 else ExtraIcon_SetIcon(g_kicqExtraIcon, contact, INVALID_HANDLE_VALUE);
 return 0;
}

// Windows XP: apply the existing registered image to ONE changed contact.
// Do not call ExtraIcon_SetAll(): it can repeatedly rebuild the entire clist.
void KicqRefreshSingleXStatus(MCONTACT hContact)
{
 if (hContact && g_kicqExtraIcon)
  KicqExtraApply((WPARAM)hContact, 0);
}

// Only update copies of this account's own UIN; never rebuild all icons.
static void KicqRefreshOwnXStatus(const char *module)
{
 if (!g_kicqExtraIcon || !module) return;
 const uint32_t ownUin = db_get_dw(0, module, "UIN", 0);
 if (!ownUin) return;
 for (MCONTACT h = db_find_first(); h; h = db_find_next(h)) {
  const char *base = Proto_GetBaseAccountName(h);
  if (base && !mir_strcmp(base, module) && db_get_dw(h, module, "UIN", 0) == ownUin)
   KicqRefreshSingleXStatus(h);
 }
}

static int CompareUsers(const KicqUser *p1, const KicqUser *p2)
{
	return p1->name.compare(p2->name);
}

CKicqProto::CKicqProto(const char *szModuleName, const wchar_t *wszUserName) :
	PROTO<CKicqProto>(szModuleName, wszUserName),
	m_arUsers(50, CompareUsers),
	m_arGroups(10),
	m_wszServer(this, "Server", _A2W(KICQ_DEFAULT_SERVER)),
	m_iPort(this, "Port", KICQ_DEFAULT_PORT),
	m_iBosMode(this, "BosMode", 0),
	m_wszBosServer(this, "BosServer", L""),
	m_iBosPortOverride(this, "BosPort", 0),
	m_wszDefaultGroup(this, "DefaultGroup", L"General"),
    m_wszFileProxy(this, "FileProxy", _A2W(KICQ_DEFAULT_FILE_PROXY)),
    m_iFileProxyPort(this, "FileProxyPort", KICQ_DEFAULT_FILE_PROXY_PORT)
{
	NETLIBUSER nlu = {};
	nlu.flags = NUF_OUTGOING | NUF_UNICODE;
	nlu.szSettingsModule = m_szModuleName;
	nlu.szDescriptiveName.w = m_tszUserName;
	m_hNetlibUser = Netlib_RegisterUser(&nlu);

	HookProtoEvent(ME_OPT_INITIALISE, &CKicqProto::OnOptionsInit);
	CreateProtoService("/XStatusEditor", &CKicqProto::OnMenuXStatus);
 CreateProtoService("/RetryAuth", &CKicqProto::OnRetryAuth);
 HookProtoEvent(ME_CLIST_PREBUILDCONTACTMENU,&CKicqProto::OnPreBuildContactMenu);
	CreateProtoService(PS_GETAVATARCAPS, &CKicqProto::OnAvatarCaps);
	CreateProtoService(PS_GETAVATARINFO, &CKicqProto::OnGetAvatarInfo);
	CreateProtoService(PS_GETMYAVATAR, &CKicqProto::OnGetMyAvatar);
	CreateProtoService(PS_SETMYAVATAR, &CKicqProto::OnSetMyAvatar);
	HookProtoEvent(ME_CLIST_PREBUILDSTATUSMENU, &CKicqProto::OnPreBuildStatusMenu);
	HookProtoEvent(ME_DB_CONTACT_SETTINGCHANGED, &CKicqProto::OnSettingChanged);
	HookProtoEvent(ME_CLIST_GROUPCHANGE, &CKicqProto::OnGroupChange);
}

CKicqProto::~CKicqProto()
{
 if(m_hRetryAuthMenu) Menu_RemoveItem(m_hRetryAuthMenu);
 StopBart();
 ShutdownOutgoingFiles();
}

void CKicqProto::OnModulesLoaded()
{
	// File transfer notifications are created by the built-in Miranda File API.
	// Do not register EVENTTYPE_FILE as DETF_MSGWINDOW here: that routes
	// the notification into TabSRMM instead of SRFile/RecvFile.
	HookProtoEvent(ME_USERINFO_INITIALISE, &CKicqProto::OnUserInfoInit);
	// statuses are not kept between sessions
	setAllContactStatuses(ID_STATUS_OFFLINE);
	// X-status presence is session-scoped. Never restore stale remote X-status
	// data from the previous session (which could render as a phantom icon).
	for (MCONTACT h = db_find_first(); h; h = db_find_next(h)) {
		const char *module = Proto_GetBaseAccountName(h);
		if (module && !mir_strcmp(module, m_szModuleName)) {
			db_unset(h, m_szModuleName, "XStatusId");
			db_unset(h, m_szModuleName, "XStatusName");
			db_unset(h, m_szModuleName, "XStatusLive");
			if (g_kicqExtraIcon) ExtraIcon_SetIcon(g_kicqExtraIcon, h, INVALID_HANDLE_VALUE);
		}
	}
 for (auto &contact : AccContacts()) {
  auto id=ContactId(contact); if(!id.empty()) setString(contact,"ScreenName",id.c_str());
 }
 {auto id=AccountId();if(!id.empty()){setString("ScreenName",id.c_str());setString("LoginId",id.c_str());}}
 if(!m_hRetryAuthMenu) {
  CMenuItem mi(&g_plugin); mi.name.a=LPGEN("Request authorization again"); mi.position=200200;
  CMStringA service(FORMAT,"%s/RetryAuth",m_szModuleName); mi.pszService=service.GetBuffer();
  m_hRetryAuthMenu=Menu_AddContactMenuItem(&mi);
 }
	EnsureXStatusMenu();
 bool registered = false;
 for (const auto &name : g_kicqIconModules)
  if (!mir_strcmp(name.c_str(), m_szModuleName)) { registered = true; break; }
 if (!registered) g_kicqIconModules.push_back(CMStringA(m_szModuleName));
 if (!g_kicqExtraIcon)
  g_kicqExtraIcon = ExtraIcon_RegisterCallback("PMLICQ_XStatus", "PMLICQ X-status", nullptr, KicqExtraRebuild, KicqExtraApply);
 // Avoid forcing a synchronous global extra-icon rebuild during startup.
 // The registered callback is invoked by the Extra Icons service when needed.
}

// A newly created account can receive OnModulesLoaded before its status-menu
// root exists. Retry when Miranda builds the status menu for the first time.
void CKicqProto::EnsureXStatusMenu()
{
 if (m_hXStatusMenu) return;
 HGENMENU root = Menu_GetProtocolRoot(this);
 if (!root) return;
 CMenuItem mi(&g_plugin);
 mi.root = root;
 mi.pszService = "/XStatusEditor";
 mi.name.a = LPGEN("X-status...");
 mi.position = 200100;
 m_hXStatusMenu = Menu_AddProtoMenuItem(&mi, m_szModuleName);
 const int ownX = getByte("XStatusId", 0);
 if (m_hXStatusMenu && ownX >= 1 && ownX <= 35)
  Menu_ModifyItem(m_hXStatusMenu, kKicqXStatuses[ownX - 1].nameRu, KicqXIcon(ownX));
}

int __cdecl CKicqProto::OnPreBuildStatusMenu(WPARAM, LPARAM)
{
 EnsureXStatusMenu();
 return 0;
}

void CKicqProto::OnShutdown()
{
	m_bTerminated = true;
	StopBart();
	ShutdownOutgoingFiles();
	ShutdownSession();
}

/////////////////////////////////////////////////////////////////////////////////////////
// contacts

std::string CKicqProto::ContactId(MCONTACT contact)
{
 const uint32_t uin = getDword(contact, "UIN");
 if (uin) return std::to_string(uin);
 return oscarid::key(getMStringA(contact, "ScreenName").c_str());
}
std::string CKicqProto::AccountId()
{
 const uint32_t uin = getDword("UIN");
 if (uin) return std::to_string(uin);
 return oscarid::key(getMStringA("LoginId").c_str());
}
MCONTACT CKicqProto::FindContact(const std::string &name)
{
 const auto id = oscarid::key(name);
 if(id.empty()) return 0;
 const auto uin=oscarid::uin(id);
 for (auto &hContact : AccContacts())
  if (uin ? getDword(hContact,"UIN")==uin : ContactId(hContact)==id) return hContact;
 return 0;
}

MCONTACT CKicqProto::CreateContact(const std::string &name, bool bTemporary)
{
 const auto id = oscarid::key(name);
 if(id.empty()) return 0;
 const auto uin = oscarid::uin(id);
	static volatile LONG traceCount = 0;
	LONG sequence = InterlockedIncrement(&traceCount);
	MCONTACT hContact = FindContact(id);
	bool trace = sequence <= 12 || (sequence % 25) == 0;
	if (trace) KicqDiagTrace("CreateContact ENTER");
	if (hContact) {
        if (trace) KicqDiagTrace("CreateContact found existing contact");
		return hContact;
    }

	KicqDiagTrace("CreateContact BEFORE db_add_contact 0.96.8");
	hContact = db_add_contact(m_szModuleName, bTemporary ? DBAC_NOTINLIST : 0);
	KicqDiagTrace(hContact ? "CreateContact db_add_contact OK" : "ERROR CreateContact db_add_contact FAILED");
	if (!hContact)
		return 0;
	const char *boundProto = Proto_GetBaseAccountName(hContact);
    bool bound = boundProto && mir_strcmp(boundProto, m_szModuleName) == 0;
    if (!bound) KicqDiagTrace("ERROR CreateContact ACCOUNT BINDING MISMATCH");
    else if (trace) KicqDiagTrace("CreateContact ACCOUNT BINDING OK");
	if(uin) setDword(hContact, "UIN", uin);
 setString(hContact, "ScreenName", id.c_str());
 if(!uin) setWString(hContact, "Nick", _A2T(id.c_str()).get());
	setWord(hContact, "Status", ID_STATUS_OFFLINE);
	if (trace) KicqDiagTrace("CreateContact EXIT (UIN/Status stored)");
	return hContact;
}

KicqUser* CKicqProto::FindUser(const std::string &name)
{
	KicqUser tmp(oscarid::key(name), 0);
	return m_arUsers.find(&tmp);
}

KicqUser* CKicqProto::AddUser(const std::string &name, MCONTACT hContact)
{
	if (auto *pUser = FindUser(name))
		return pUser;

	auto *pUser = new KicqUser(oscarid::key(name), hContact);
	m_arUsers.insert(pUser);
	return pUser;
}

KicqGroup* CKicqProto::FindGroup(int id)
{
	for (auto &it : m_arGroups)
		if (it->id == id)
			return it;
	return nullptr;
}

KicqGroup* CKicqProto::FindGroup(const wchar_t *name)
{
	for (auto &it : m_arGroups)
		if (!it->bNotInList && !mir_wstrcmpi(it->name, name))
			return it;
	return nullptr;
}

int CKicqProto::NewSsiId()
{
	// Jasmine: a random 16-bit id above 4096 that the roster does not use yet
	for (int tries = 0; tries < 100; tries++) {
		int id = (rand() & 0x6FFF) + 4096;
		bool bUsed = FindGroup(id) != nullptr;
		for (auto &it : m_arUsers)
			if (it->ssiId == id)
				bUsed = true;
		if (!bUsed)
			return id;
	}
	return (rand() & 0x6FFF) + 4096;
}

// the server group a contact should go to: the one named like its clist group, else the
// default group of the account (0 when that group does not exist on the server yet)
int CKicqProto::ResolveGroupFor(MCONTACT hContact)
{
	ptrW wszGroup(Clist_GetGroup(hContact));
	if (mir_wstrlen(wszGroup) == 0)
		wszGroup = mir_wstrdup(m_wszDefaultGroup);

	mir_cslock lck(m_csRoster);
	if (auto* pGroup = FindGroup(wszGroup.get()))
		return pGroup->id;
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// statuses

int CKicqProto::MirandaToIcqStatus(int iStatus)
{
	switch (iStatus) {
	case ID_STATUS_AWAY:      return ICQ_STATUS_AWAY;
	case ID_STATUS_NA:        return ICQ_STATUS_NA;
	case ID_STATUS_DND:       return ICQ_STATUS_DND;
	case ID_STATUS_OCCUPIED:  return ICQ_STATUS_OCCUPIED;
	case ID_STATUS_FREECHAT:  return ICQ_STATUS_FFC;
	case ID_STATUS_INVISIBLE: return ICQ_STATUS_INVISIBLE;
	}
	return ICQ_STATUS_ONLINE;
}

int CKicqProto::IcqToMirandaStatus(int iIcqStatus)
{
	iIcqStatus &= 0xFFFF;
	if (iIcqStatus & ICQ_STATUS_INVISIBLE)
		return ID_STATUS_INVISIBLE;
	if (iIcqStatus & ICQ_STATUS_DND)
		return ID_STATUS_DND;
	if (iIcqStatus & ICQ_STATUS_OCCUPIED)
		return ID_STATUS_OCCUPIED;
	if (iIcqStatus & ICQ_STATUS_NA)
		return ID_STATUS_NA;
	if (iIcqStatus & ICQ_STATUS_AWAY)
		return ID_STATUS_AWAY;
	if (iIcqStatus & ICQ_STATUS_FFC)
		return ID_STATUS_FREECHAT;
	return ID_STATUS_ONLINE;
}

CMStringW CKicqProto::GetOwnAwayText()
{
	char szSetting[40];
	mir_snprintf(szSetting, "AwayMsg%d", m_iStatus);
	return getMStringW(szSetting);
}

void CKicqProto::SendStatus()
{
	// Status changes only need TLV 0x0006 (length 4). Avoid resending
	// legacy DC-info TLVs, which may be rejected by modern OSCAR servers.
	const int oscarStatus = MirandaToIcqStatus(m_iStatus);
	debugLogA("OSCAR status switch: miranda=%d, oscar=0x%04X, flags=256", m_iStatus, oscarStatus);
	// Match the known-working initial presence packet.
	Send(OscarPackets::setStatus(oscarStatus, 256));
	SendXStatus();
}

int CKicqProto::SetStatus(int iNewStatus)
{
	debugLogA("SetStatus %d (current %d)", iNewStatus, m_iStatus);

	if (iNewStatus != ID_STATUS_OFFLINE && !(GetCaps(PFLAGNUM_2) & Proto_Status2Flag(iNewStatus)))
		iNewStatus = ID_STATUS_ONLINE;

	int iOldStatus = m_iStatus;
	m_iDesiredStatus = iNewStatus;

	if (iNewStatus == ID_STATUS_OFFLINE) {
		ShutdownSession();
		if (!m_bOnline && !IsStatusConnecting(m_iStatus) && m_iStatus != ID_STATUS_OFFLINE) {
			m_iStatus = ID_STATUS_OFFLINE;
			ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)iOldStatus, m_iStatus);
		}
		return 0;
	}

	if (m_bOnline) {
		m_iStatus = iNewStatus;
		SendStatus();
		ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)iOldStatus, m_iStatus);
		return 0;
	}

	if (IsStatusConnecting(m_iStatus))
		return 0;

	if (AccountId().empty() || getMStringA("Password").IsEmpty()) {
		ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_BADUSERID);
		return 0;
	}

	m_bTerminated = false;
	m_iStatus = ID_STATUS_CONNECTING;
	ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)iOldStatus, m_iStatus);
	ForkThread(&CKicqProto::ServerThread);
	return 0;
}

int CKicqProto::SetAwayMsg(int iStatus, const wchar_t *msg)
{
	char szSetting[40];
	mir_snprintf(szSetting, "AwayMsg%d", iStatus);
	setWString(szSetting, msg ? msg : L"");

	// Do not send legacy ICQ mood/away-text TLV 0x001D during a live
	// status transition. Miranda invokes SetAwayMsg for away statuses, and
	// this extra packet can cause Open OSCAR Server to reject the session.
	// The message remains stored locally; wire support can be added later
	// after validating the server's TLV 0x001D decoder.
	if (m_bOnline && iStatus == m_iStatus)
		debugLogA("OSCAR: away message updated locally (network TLV 0x001D suppressed)");
	return 0;
}

void __cdecl CKicqProto::GetAwayMsgThread(void *param)
{
	// the server sends the text with the presence, so it is already known
	MCONTACT hContact = (UINT_PTR)param;
	CMStringW wszMsg(db_get_wsm(hContact, "CList", "StatusMsg"));
	ProtoBroadcastAck(hContact, ACKTYPE_AWAYMSG, ACKRESULT_SUCCESS, (HANDLE)1, (LPARAM)wszMsg.c_str());
}

HANDLE CKicqProto::GetAwayMsg(MCONTACT hContact)
{
	ForkThread(&CKicqProto::GetAwayMsgThread, (void *)hContact);
	return (HANDLE)1;
}

/////////////////////////////////////////////////////////////////////////////////////////
// capabilities

INT_PTR CKicqProto::GetCaps(int type, MCONTACT)
{
	switch (type) {
	case PFLAGNUM_1:
		return PF1_FILE | PF1_IM | PF1_SERVERCLIST | PF1_AUTHREQ | PF1_BASICSEARCH | PF1_ADDSEARCHRES | PF1_MODEMSGRECV | PF1_MODEMSGSEND;

	case PFLAGNUM_2:
		return PF2_ONLINE | PF2_SHORTAWAY | PF2_LONGAWAY | PF2_LIGHTDND | PF2_HEAVYDND | PF2_FREECHAT | PF2_INVISIBLE;

	case PFLAGNUM_3:
		return PF2_SHORTAWAY | PF2_LONGAWAY | PF2_LIGHTDND | PF2_HEAVYDND | PF2_FREECHAT;

	case PFLAGNUM_4:
		return PF4_SUPPORTTYPING | PF4_IMSENDOFFLINE | PF4_AVATARS;

	case PFLAG_UNIQUEIDTEXT:
		return (INT_PTR)TranslateT("UIN / Screen Name");

	case PFLAG_MAXLENOFMESSAGE:
		return 2048;
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// messaging

int CKicqProto::SendMsg(MCONTACT hContact, MEVENT, const char *pszSrc)
{
	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	int seq = InterlockedIncrement((long *)&m_msgSeq);
	if (!m_bOnline || peer.empty()) {
		ProtoBroadcastAsync(hContact, ACKTYPE_MESSAGE, ACKRESULT_FAILED, (HANDLE)seq, (LPARAM)TranslateT("You are offline"));
		return seq;
	}

	CMStringW text(Utf2T(pszSrc).get());
	text.Replace(L"\n", L"\r\n");

	const char *szUin = peer.c_str();

	bytes_t cookie(8, 0);
	uint64_t id = GetTickCount() ^ (uint64_t(rand()) << 32);
	for (int i = 0; i < 8; i++)
		cookie[i] = char(id >> (8 * (7 - i)));

	bool bRelay = false, bUtf8 = false, bOnline = false;
	{
		mir_cslock lck(m_csRoster);
		if (auto *pUser = FindUser(peer)) {
			bOnline = pUser->bOnline;
			bUtf8 = pUser->bUtf8;
			bRelay = pUser->ssiId != 0 && pUser->bAuthorized && pUser->bOnline && pUser->bServerRelay;
		}
	}

	uint32_t reqId;
	{
		mir_cslock lck(m_csPending);
		reqId = m_nextMsgReq++;
		if (m_nextMsgReq > 0x7FFFFFF0)
			m_nextMsgReq = 0x1000;
		m_pendingMsgs[reqId] = { peer, hContact, seq };
	}

	if (uin && getDword("UIN") && bRelay)
		Send(OscarPackets::messageChannel2(szUin, text, bUtf8, cookie, reqId));
	else if (uin && getDword("UIN") && !bOnline) {
		// kicq.ru's offline store keeps only bytes 0x01-0x7F and stops at the first 0x00:
		// ASCII text survives as charset 0, anything else only as UCS-2 where only characters
		// >= U+0100 get through. Spaces become EN SPACE (U+2002) so that whole Cyrillic
		// sentences survive; digits and Latin letters inside them cannot.
		bool bAscii = true;
		for (int i = 0; i < text.GetLength() && bAscii; i++)
			if (text[i] >= 0x80)
				bAscii = false;

		if (bAscii)
			Send(OscarPackets::messageChannel1(szUin, OscarText::toCp1251(text), 0, cookie, reqId));
		else {
			CMStringW tmp(text);
			tmp.Replace(' ', 0x2002);
			Send(OscarPackets::messageChannel1(szUin, OscarText::toUcs2(tmp), 2, cookie, reqId));
		}
	}
	else Send(OscarPackets::messageChannel1(szUin, OscarText::toUcs2(text), 2, cookie, reqId));

	return seq;
}

int CKicqProto::UserIsTyping(MCONTACT hContact, int type)
{
	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	if (!m_bOnline || peer.empty() || m_iStatus == ID_STATUS_INVISIBLE)
		return 0;

	const char *szUin = peer.c_str();
	Send(OscarPackets::typingNotify(szUin, type == PROTOTYPE_SELFTYPING_ON ? 2 : 0));
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// roster edits

void CKicqProto::SsiAddContact(MCONTACT hContact, int groupId)
{
	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	const char *szUin = peer.c_str();

	CMStringW wszNick(db_get_wsm(hContact, "CList", "MyHandle"));
	if (wszNick.IsEmpty())
		wszNick = getMStringW(hContact, "Nick");
	if (wszNick.IsEmpty())
		wszNick = _A2T(szUin).get();

	if(peer.empty()) return;
 m_pendAdd.id = peer;
	m_pendAdd.name = wszNick;
	m_pendAdd.groupId = groupId;
	m_pendAdd.hContact = hContact;
	{
		mir_cslock lck(m_csRoster);
		m_pendAdd.ssiId = NewSsiId();
	}

	Send(OscarPackets::ssiEditStart());
	Send(OscarPackets::addContact(szUin, wszNick, groupId, m_pendAdd.ssiId, false));
	Send(OscarPackets::ssiEditEnd());
}

MCONTACT CKicqProto::AddToList(int flags, PROTOSEARCHRESULT *psr)
{
	if(!psr || ((psr->flags & PSR_UNICODE) ? psr->id.w==nullptr : psr->id.a==nullptr))return 0;
 const auto peer = oscarid::key((psr->flags & PSR_UNICODE) ? T2Utf(psr->id.w).get() : psr->id.a);
	if (peer.empty())
		return 0;

	bool bTemporary = (flags & PALF_TEMPORARY) != 0;
	MCONTACT hContact = CreateContact(peer, bTemporary);
 if(!hContact) return 0;
	if (psr->nick.w && (psr->flags & PSR_UNICODE))
		setWString(hContact, "Nick", psr->nick.w);

	if (!bTemporary) {
		Contact::PutOnList(hContact);
		Contact::Hide(hContact, false);

		if (m_bOnline) {
			int groupId = ResolveGroupFor(hContact);
			if (groupId)
				SsiAddContact(hContact, groupId);
			else {
				// the group does not exist on the server yet: create it first
				ptrW wszGroup(Clist_GetGroup(hContact));
				m_wszAddAfterGroup = mir_wstrlen(wszGroup) ? wszGroup.get() : (wchar_t *)m_wszDefaultGroup;
				m_hAddAfterGroup = hContact;
				m_pendGroup.name = m_wszAddAfterGroup;
				{
					mir_cslock lck(m_csRoster);
					m_pendGroup.groupId = NewSsiId();
				}
				Send(OscarPackets::ssiEditStart());
				Send(OscarPackets::addGroup(m_pendGroup.name, m_pendGroup.groupId));
				Send(OscarPackets::ssiEditEnd());
			}
		}
	}
	if (m_bOnline) ScheduleBartRefresh(peer, true);
	return hContact;
}

MCONTACT CKicqProto::AddToListByEvent(int flags, int, MEVENT hDbEvent)
{
	DB::EventInfo dbei(hDbEvent);
	if (!dbei || mir_strcmp(dbei.szModule, m_szModuleName))
		return 0;
	if (dbei.eventType != EVENTTYPE_AUTHREQUEST && dbei.eventType != EVENTTYPE_ADDED)
		return 0;

	MCONTACT hContact = DbGetAuthEventContact(&dbei);
	if (hContact == 0)
		return 0;

	CMStringW wszUin(_A2T(ContactId(hContact).c_str()).get());
	PROTOSEARCHRESULT psr = {};
	psr.cbSize = sizeof(psr);
	psr.flags = PSR_UNICODE;
	psr.id.w = wszUin.GetBuffer();
	return AddToList(flags, &psr);
}

bool CKicqProto::OnContactDeleted(MCONTACT hContact, uint32_t flags)
{
	if (!(flags & CDF_DEL_CONTACT) || !m_bOnline)
		return true;

	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	int groupId = 0, ssiId = 0;
	{
		mir_cslock lck(m_csRoster);
		if (auto *pUser = FindUser(peer)) {
			groupId = pUser->groupId;
			ssiId = pUser->ssiId;
			m_arUsers.remove(pUser);
		}
	}

	if (ssiId != 0) {
		const char *szUin = peer.c_str();
		m_pendDel.uin = uin;
		Send(OscarPackets::ssiEditStart());
		Send(OscarPackets::deleteContact(szUin, groupId, ssiId));
		Send(OscarPackets::ssiEditEnd());
	}
	return true;
}

// renames a contact on the server when its clist name changes
int CKicqProto::OnSettingChanged(WPARAM hContact, LPARAM lParam)
{
	auto *cws = (DBCONTACTWRITESETTING *)lParam;
	// The account-level X-status changes even when no buddy presence arrives.
	if (!hContact && cws && cws->szModule && cws->szSetting &&
		!mir_strcmp(cws->szModule, m_szModuleName) &&
		!mir_strcmp(cws->szSetting, "XStatusId"))
		KicqRefreshOwnXStatus(m_szModuleName);
	// Presence transitions also invalidate the extra-icon slot. In particular,
	// a contact can go offline without receiving a USER_OFFLINE SNAC.
	// Do not rebuild all icons: refresh only this contact on a status change.
	// XP diagnostic: intentionally suppress synchronous ExtraIcon_SetAll on Status DB events.
	// X-status DB fields and message handling remain intact.
	if (hContact == 0 && cws && cws->szModule && cws->szSetting &&
		!mir_strcmp(cws->szModule, m_szModuleName) && !m_bProfileSync) {
		const char *keys[] = { "Nick", "FirstName", "LastName", "e-mail", "City", "State",
			"Phone", "Fax", "Street", "Cellular", "ZIP", "Country", "GMTOffset", "PublishEmail" };
		for (const char *key : keys)
			if (!mir_strcmp(cws->szSetting, key)) { SendBasicProfile(); break; }
        const char *extended[] = { "About", "Company", "CompanyDepartment", "CompanyPosition",
          "BirthYear", "BirthMonth", "BirthDay", "Homepage" };
        for (const char *key : extended)
            if (!mir_strcmp(cws->szSetting, key)) { SendExtendedProfile(key); break; }
	}
	if (hContact == 0 || !m_bOnline || mir_strcmp(cws->szModule, "CList") || mir_strcmp(cws->szSetting, "MyHandle"))
		return 0;
	if (mir_strcmp(Proto_GetBaseAccountName(hContact), m_szModuleName))
		return 0;

	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	CMStringW wszNick(db_get_wsm(hContact, "CList", "MyHandle"));
	if (wszNick.IsEmpty())
		wszNick = _A2T(peer.c_str()).get();

	int groupId = 0, ssiId = 0;
	bool bAuthorized = true;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(peer);
		if (pUser == nullptr || pUser->ssiId == 0)
			return 0;
		groupId = pUser->groupId;
		ssiId = pUser->ssiId;
		bAuthorized = pUser->bAuthorized;
	}

	if (getMStringW(hContact, "SrvNick") == wszNick)
		return 0; // the change came from the server

	const char *szUin = peer.c_str();
	m_pendRename.id = peer;
	m_pendRename.name = wszNick;
	Send(OscarPackets::ssiEditStart());
	Send(OscarPackets::renameContact(szUin, wszNick, groupId, ssiId, !bAuthorized));
	Send(OscarPackets::ssiEditEnd());
	return 0;
}

// moves a contact between server groups when it is dragged to another clist group
int CKicqProto::OnGroupChange(WPARAM hContact, LPARAM lParam)
{
	auto *pParam = (CLISTGROUPCHANGE *)lParam;
	if (hContact == 0 || !m_bOnline || pParam->pszNewName == nullptr)
		return 0;
	if (mir_strcmp(Proto_GetBaseAccountName(hContact), m_szModuleName))
		return 0;

	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	int oldGroup = 0, ssiId = 0, newGroup = 0;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(peer);
		if (pUser == nullptr || pUser->ssiId == 0)
			return 0;

		auto *pGroup = FindGroup(pParam->pszNewName);
		if (pGroup == nullptr || pGroup->id == pUser->groupId)
			return 0;

		oldGroup = pUser->groupId;
		ssiId = pUser->ssiId;
		newGroup = pGroup->id;
		pUser->ssiId = 0;
	}

	// OSCAR has no "move": the item is deleted from the old group and added to the new one
	const char *szUin = peer.c_str();
	m_pendDel.uin = 0;
	Send(OscarPackets::ssiEditStart());
	Send(OscarPackets::deleteContact(szUin, oldGroup, ssiId));
	Send(OscarPackets::ssiEditEnd());
	SsiAddContact(hContact, newGroup);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// authorization

int CKicqProto::AuthRecv(MCONTACT, DB::EventInfo &dbei)
{
	return Proto_AuthRecv(m_szModuleName, dbei);
}

int CKicqProto::AuthRequest(MCONTACT hContact, const wchar_t *szMessage)
{
	const auto peer = ContactId(hContact);
 uint32_t uin = oscarid::uin(peer);
	if (!m_bOnline || peer.empty())
		return 1;

	const char *szUin = peer.c_str();
	Send(OscarPackets::futureAuthGrant(szUin));
	Send(OscarPackets::authRequest(szUin, szMessage && mir_wstrlen(szMessage) ? szMessage : TranslateT("Please authorize me")));
 setByte(hContact,"Auth",1);
 {mir_cslock lck(m_csRoster);if(auto *pUser=FindUser(peer))pUser->bAuthorized=false;}
	return 0;
}

static MCONTACT AuthEventContact(const char *szModule, MEVENT hDbEvent)
{
	DB::EventInfo dbei(hDbEvent);
	if (!dbei || mir_strcmp(dbei.szModule, szModule) || dbei.eventType != EVENTTYPE_AUTHREQUEST)
		return 0;
	return DbGetAuthEventContact(&dbei);
}

int CKicqProto::Authorize(MEVENT hDbEvent)
{
	MCONTACT hContact = AuthEventContact(m_szModuleName, hDbEvent);
	const auto peer = hContact ? ContactId(hContact) : std::string();
	if (!m_bOnline || peer.empty())
		return 1;

	const char *szUin = peer.c_str();
	Send(OscarPackets::authReply(szUin, true));
 delSetting(hContact,"Grant");
	return 0;
}

int CKicqProto::AuthDeny(MEVENT hDbEvent, const wchar_t *)
{
	MCONTACT hContact = AuthEventContact(m_szModuleName, hDbEvent);
	const auto peer = hContact ? ContactId(hContact) : std::string();
	if (!m_bOnline || peer.empty())
		return 1;

	const char *szUin = peer.c_str();
	Send(OscarPackets::authReply(szUin, false));
 delSetting(hContact,"Grant");
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// user details & search

uint32_t CKicqProto::RequestUserInfo(uint32_t uin, MCONTACT hContact, HANDLE hSearch)
{
	uint32_t reqId;
	{
		mir_cslock lck(m_csPending);
		reqId = m_nextInfoReq++;
		auto &req = m_infoRequests[reqId];
		req.hContact = hContact;
		req.hSearch = hSearch;
		req.uin = uin;
		req.started = time(0);
	}
	Send(OscarPackets::userInfoRequest(atoi(m_szUin), uin, reqId & 0xFFFF, reqId));
	return reqId;
}

// Refresh the owner profile from OSCAR. The existing Meta reply handler
// updates the account's DB settings, which the UserInfo editor reads.
void CKicqProto::RefreshOwnProfile()
{
 if (!m_bOnline) return;
 const uint32_t uin = getDword("UIN");
 if (uin) RequestUserInfo(uin, 0, nullptr);
}

// Commit a UserInfo page as a single logical edit. Suppress the normal
// per-setting callback while persisting fields, then send only the Meta
// sections that actually changed.
void CKicqProto::SaveOwnProfileFields(
 const std::vector<std::pair<std::string, std::wstring>> &changes, bool extended)
{
 bool basicChanged = false, workChanged = false, moreChanged = false, aboutChanged = false;
 m_bProfileSync = true;
 for (const auto &entry : changes) {
  if (getMStringW(entry.first.c_str()) == entry.second.c_str()) continue;
  db_set_ws(0, m_szModuleName, entry.first.c_str(), entry.second.c_str());
  if (!extended) basicChanged = true;
  else if (entry.first == "About") aboutChanged = true;
  else if (entry.first == "Company" || entry.first == "CompanyDepartment" || entry.first == "CompanyPosition") workChanged = true;
  else moreChanged = true;
 }
 m_bProfileSync = false;
 if (basicChanged) SendBasicProfile();
 if (aboutChanged) SendExtendedProfile("About");
 if (workChanged) SendExtendedProfile("Company");
 if (moreChanged) SendExtendedProfile("Homepage");
}

// The basic ICQ profile (SNAC 15/02, Meta 07D0/03EA).
// User edits are saved by Miranda in this account's database module first.
// CP1251 is the interoperable encoding for legacy ICQ/QIP profile strings.
static bytes_t EncodeIcqProfileField(const CMStringW &value)
{
 if (value.IsEmpty()) return bytes_t();
 int n = WideCharToMultiByte(1251, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
 if (n <= 1) return bytes_t();
 std::string buffer(size_t(n), '\0');
 WideCharToMultiByte(1251, 0, value.c_str(), -1, &buffer[0], n, nullptr, nullptr);
 buffer.resize(size_t(n) - 1);
 return buffer;
}

void CKicqProto::SendBasicProfile()
{
 uint32_t uin = getDword("UIN");
 if (!m_bOnline || !uin) return;
 const char *keys[] = { "Nick", "FirstName", "LastName", "e-mail", "City",
  "State", "Phone", "Fax", "Street", "Cellular", "ZIP" };
 std::vector<bytes_t> fields;
 for (const char *key : keys)
  fields.push_back(EncodeIcqProfileField(getMStringW(key)));
 uint32_t id = ++m_nextInfoReq;
 Send(OscarPackets::setBasicProfile(uin, id & 0xFFFF, id, fields,
  getWord("Country"), getByte("GMTOffset"), getByte("PublishEmail") != 0));
 debugLogA("ICQ basic profile update sent, req=%u", id);
}

// Each ICQ Meta update uses its own request ID; avoid sending unrelated profile
// sections when one edit control changes. The server is authoritative.
void CKicqProto::SendExtendedProfile(const char *changedKey)
{
 if (!m_bOnline || !getDword("UIN")) return;
 const auto field = [this](const char *name) {
  bytes_t encoded = EncodeIcqProfileField(getMStringW(name));
  if (encoded.size() > 1023) encoded.resize(1023);
  return encoded;
 };
 const auto appendField = [](OscarWriter &w, const bytes_t &encoded) {
  w.u16le(int(encoded.size()) + 1).bytes(encoded).u8(0);
 };
 OscarWriter payload;
 uint16_t operation = 0;
 if (!mir_strcmp(changedKey, "About")) {
  operation = 0x0406; // SetNotes
  appendField(payload, field("About"));
 }
 else if (!mir_strcmp(changedKey, "Company") ||
          !mir_strcmp(changedKey, "CompanyDepartment") ||
          !mir_strcmp(changedKey, "CompanyPosition")) {
  operation = 0x03F3; // SetWorkInfo
  // Work: city, state, phone, fax, address, zip, country,
  // company, department, position, occupation, homepage.
  const char *keys[] = { "WorkCity", "WorkState", "WorkPhone", "WorkFax", "WorkStreet", "WorkZIP" };
  for (const char *key : keys) appendField(payload, field(key));
  payload.u16le(getWord("WorkCountry"));
  appendField(payload, field("Company"));
  appendField(payload, field("CompanyDepartment"));
  appendField(payload, field("CompanyPosition"));
  payload.u16le(getWord("Occupation"));
  appendField(payload, field("WorkHomepage"));
 }
 else if (!mir_strcmp(changedKey, "BirthYear") || !mir_strcmp(changedKey, "BirthMonth") ||
          !mir_strcmp(changedKey, "BirthDay") || !mir_strcmp(changedKey, "Homepage")) {
  operation = 0x03FD; // SetMoreInfo
  payload.u16le(getWord("Age"));
  uint8_t gender = getByte("Gender");
  payload.u8(gender == 'F' ? 1 : gender == 'M' ? 2 : 0);
  appendField(payload, field("Homepage"));
  payload.u16le(_wtoi(getMStringW("BirthYear")));
  payload.u8(_wtoi(getMStringW("BirthMonth"))).u8(_wtoi(getMStringW("BirthDay")));
  payload.u8(0).u8(0).u8(0); // languages, unspecified
 }
 if (!operation) return;
 const uint32_t id = ++m_nextInfoReq;
 Send(OscarPackets::setExtendedProfile(getDword("UIN"), id & 0xffff, id, operation, payload.data()));
 debugLogA("ICQ profile extended update type=%04X req=%u", operation, id);
}

int CKicqProto::GetInfo(MCONTACT hContact, int)
{
	uint32_t uin = hContact ? getDword(hContact, "UIN") : getDword("UIN");
	if (!m_bOnline || uin == 0 || !getDword("UIN"))
		return 1;

	RequestUserInfo(uin, hContact, nullptr);
	return 0;
}

struct KicqScreenSearch { std::string id; HANDLE handle; };
void __cdecl CKicqProto::SearchScreenName(void *param)
{
 std::unique_ptr<KicqScreenSearch> search(static_cast<KicqScreenSearch*>(param));
 // Direct add by exact OSCAR name; no claim that the account exists.
 Sleep(100);
 CMStringW id(_A2T(search->id.c_str()).get());
 PROTOSEARCHRESULT psr = {}; psr.cbSize=sizeof(psr); psr.flags=PSR_UNICODE;
 psr.id.w=id.GetBuffer(); psr.nick.w=id.GetBuffer();
 ProtoBroadcastAck(0,ACKTYPE_SEARCH,ACKRESULT_DATA,search->handle,(LPARAM)&psr);
 ProtoBroadcastAck(0,ACKTYPE_SEARCH,ACKRESULT_SUCCESS,search->handle,0);
}
HANDLE CKicqProto::SearchBasic(const wchar_t *id)
{
 if (!m_bOnline || !id) return nullptr;
 const auto peer=oscarid::key(T2Utf(id).get());
 if(peer.empty()) return nullptr;
 const uint32_t uin=oscarid::uin(peer);
 static long searchId=0;
 HANDLE hSearch=(HANDLE)(INT_PTR)InterlockedIncrement(&searchId);
 if(uin && getDword("UIN")) RequestUserInfo(uin,0,hSearch);
 else ForkThread(&CKicqProto::SearchScreenName,new KicqScreenSearch{peer,hSearch});
 return hSearch;
}

// X-status selector with bilingual names and local title/description editor.
// Presence capability is sent through SNAC(2,4), not the ordinary status TLV.
void CKicqProto::SendXStatus()
{
 if (!m_bOnline) return;
 const int id = getByte("XStatusId", 0);
 const int count = int(sizeof(kKicqXStatuses) / sizeof(kKicqXStatuses[0]));
 const char *guid = id > 0 && id <= count ? kKicqXStatuses[id - 1].guid : nullptr;
 Send(OscarPackets::setUserInfo(1, 0, 0, guid, getByte("ClientIdPreset", 0)));
}

INT_PTR __cdecl CKicqProto::OnMenuXStatus(WPARAM, LPARAM)
{
 DialogBoxParamW(g_plugin.getInst(), MAKEINTRESOURCEW(IDD_KICQ_XSTATUS), nullptr, XStatusDlgProc, (LPARAM)this);
 return 0;
}

// Draw QIP's original 16x16 sprite cells beside the translated status names.
static void KicqDrawXStatusItem(HWND hwnd, const DRAWITEMSTRUCT *di)
{
 if (di->itemID == (UINT)-1) return;
 HDC dc = di->hDC;
 RECT rc = di->rcItem;
 const bool selected = (di->itemState & ODS_SELECTED) != 0;
 SetBkColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
 SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
 HBRUSH bg = GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
 FillRect(dc, &rc, bg);
  if (di->itemID > 0 && di->itemID <= 34) {
   HICON icon = KicqXIcon((int)di->itemID);
   if (icon)
    DrawIconEx(dc, rc.left + 2, rc.top + (rc.bottom - rc.top - 16) / 2,
     icon, 16, 16, 0, nullptr, DI_NORMAL);
  }
 wchar_t label[256] = {};
 SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)label);
 RECT textRc = rc;
 textRc.left += 22;
 DrawTextW(dc, label, -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
 if (di->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
}

static void KicqFillXStatusNames(HWND hwnd, bool russian)
{
 HWND combo = GetDlgItem(hwnd, IDC_KICQ_XSTATUS_LIST);
 int selected = (int)SendMessage(combo, CB_GETCURSEL, 0, 0);
 SendMessage(combo, CB_RESETCONTENT, 0, 0);
 SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(russian ? L"Нет (убрать X-статус)" : L"None (clear X-status)"));
 const int visibleCount = 34;
 for (int i = 0; i < visibleCount; ++i)
  SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(russian ? kKicqXStatuses[i].nameRu : kKicqXStatuses[i].name));
 SendMessage(combo, CB_SETCURSEL, selected < 0 ? 0 : selected, 0);
}

INT_PTR CALLBACK CKicqProto::XStatusDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
 if (msg == WM_INITDIALOG) {
  auto *proto = reinterpret_cast<CKicqProto*>(lp);
  SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)proto);
  HWND lang = GetDlgItem(hwnd, IDC_KICQ_XSTATUS_LANG);
  SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)L"Русский");
  SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)L"English");
  int language = proto->getByte("XStatusLanguage", 0) == 1 ? 1 : 0;
  SendMessage(lang, CB_SETCURSEL, language, 0);
  KicqFillXStatusNames(hwnd, language == 0);
  int id = proto->getByte("XStatusId", 0);
  if (id < 0 || id > 34) id = 0;
  SendDlgItemMessage(hwnd, IDC_KICQ_XSTATUS_LIST, CB_SETCURSEL, id, 0);
  CMStringW title = proto->getMStringW("XStatusName");
  CMStringW desc = proto->getMStringW("XStatusMsg");
  SetDlgItemTextW(hwnd, IDC_KICQ_XSTATUS_TITLE, title.c_str());
  SetDlgItemTextW(hwnd, IDC_KICQ_XSTATUS_DESC, desc.c_str());
  return TRUE;
 }
 if (msg == WM_DRAWITEM && wp == IDC_KICQ_XSTATUS_LIST) {
  KicqDrawXStatusItem(hwnd, reinterpret_cast<DRAWITEMSTRUCT*>(lp));
  return TRUE;
 }
 if (msg == WM_COMMAND) {
  if (LOWORD(wp) == IDC_KICQ_XSTATUS_LANG && HIWORD(wp) == CBN_SELCHANGE) {
   int language = (int)SendDlgItemMessage(hwnd, IDC_KICQ_XSTATUS_LANG, CB_GETCURSEL, 0, 0);
   KicqFillXStatusNames(hwnd, language == 0);
   return TRUE;
  }
  if (LOWORD(wp) == IDOK) {
   auto *proto = reinterpret_cast<CKicqProto*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
   int id = (int)SendDlgItemMessage(hwnd, IDC_KICQ_XSTATUS_LIST, CB_GETCURSEL, 0, 0);
   if (id == CB_ERR) id = 0;
   int language = (int)SendDlgItemMessage(hwnd, IDC_KICQ_XSTATUS_LANG, CB_GETCURSEL, 0, 0);
   wchar_t title[256] = {}, desc[1024] = {};
   GetDlgItemTextW(hwnd, IDC_KICQ_XSTATUS_TITLE, title, _countof(title));
   GetDlgItemTextW(hwnd, IDC_KICQ_XSTATUS_DESC, desc, _countof(desc));
   proto->setByte("XStatusLanguage", (uint8_t)(language == 1 ? 1 : 0));
   proto->setByte("XStatusId", (uint8_t)id);
   proto->setWString("XStatusName", title);
   proto->setWString("XStatusMsg", desc);
   proto->SendXStatus();
   KicqRefreshOwnXStatus(proto->m_szModuleName);
   // Update the protocol menu to show the selected X-status.
   if (proto->m_hXStatusMenu) {
    const wchar_t *label = id > 0 && id <= 34 ? (language == 0 ? kKicqXStatuses[id-1].nameRu : kKicqXStatuses[id-1].name) : L"X-status...";
    Menu_ModifyItem(proto->m_hXStatusMenu, label, id ? KicqXIcon(id) : nullptr);
   }
   EndDialog(hwnd, IDOK);
   return TRUE;
  }
  if (LOWORD(wp) == IDCANCEL) { EndDialog(hwnd, IDCANCEL); return TRUE; }
 }
 return FALSE;
}

// Select the IPv4 used to reach the OSCAR server; does not send any UDP data.
// This is a local interface address, not a NAT-discovered public address.
static uint32_t Oft2LocalIPv4(const char *host)
{
 // The earlier build used a hard-coded OSCAR address regardless of the
 // configured server. Choose the interface route to the actual server.
 // This is NOT public-IP discovery: NAT still requires a reachable relay.
 if (!host || !*host) return 0;
 addrinfo hints = {};
 hints.ai_family = AF_INET;
 hints.ai_socktype = SOCK_DGRAM;
 addrinfo *addresses = nullptr;
 if (getaddrinfo(host, "5190", &hints, &addresses) != 0 || !addresses)
  return 0;
 uint32_t ip = 0;
 for (addrinfo *entry = addresses; entry; entry = entry->ai_next) {
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) continue;
  if (connect(sock, entry->ai_addr, (int)entry->ai_addrlen) != SOCKET_ERROR) {
   sockaddr_in local = {};
   int len = sizeof(local);
   if (getsockname(sock, (sockaddr *)&local, &len) == 0)
    ip = ntohl(local.sin_addr.s_addr);
  }
  closesocket(sock);
  if (ip) break;
 }
 freeaddrinfo(addresses);
 return ip;
}

// Optional direct OFT2 public IPv4 advertised to the peer. A machine behind
// NAT needs a forwarding rule for Oft2ListenPort as well. Without an override
// we keep legacy route selection but explicitly warn when it is non-routable.
static uint32_t Oft2AdvertisedIPv4(const wchar_t *external, const char *server)
{
 if (external && *external) {
  T2Utf utf8(external);
  const unsigned long net = inet_addr(utf8);
  if (net != INADDR_NONE && net != INADDR_ANY)
   return ntohl(net);
  return 0; // Invalid explicit override: never silently advertise another IP.
 }
 return Oft2LocalIPv4(server);
}

static bool Oft2IsPrivateIPv4(uint32_t ip)
{
 const unsigned a = ip >> 24, b = (ip >> 16) & 255;
 return !ip || a == 0 || a == 10 || a == 127 || a >= 224 ||
        (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) ||
        (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127);
}

// Outgoing direct OFT2 sender. The listener is created BEFORE the ICBM offer.
// OSCAR clients behind NAT may require proxy/relay negotiation, not available here.
static bool Oft2SendProgress(uint64_t done, uint64_t, void *ctx)
{
 auto *f = (CKicqProto::OutgoingFile *)ctx;
 if (InterlockedCompareExchange(&f->cancelled, 0, 0)) return false;
 // Callback also updates the Miranda file-transfer progress dialog.
 return true;
}

// ARS may fail before an OSCAR offer is sent. In that case try a fresh
// direct OFT2 offer using the SAME transfer handle and cookie. Never use this
// after the relay offer has been sent, as the peer may still accept it.
bool CKicqProto::StartDirectFileFallback(const std::shared_ptr<OutgoingFile> &f)
{
 if (!f || InterlockedCompareExchange(&f->cancelled, 0, 0) || !m_bOnline)
  return false;
 if (!f->listener.open(INADDR_ANY, getWord("Oft2ListenPort", 0))) {
  debugLogA("OFT2 fallback: cannot open TCP listener");
  return false;
 }
 const CMStringW configuredPublicIp = getMStringW("Oft2ExternalIP");
 const uint32_t ip = Oft2AdvertisedIPv4(configuredPublicIp.c_str(), T2Utf(m_wszServer));
 const bytes_t cap = OscarText::unhex(CAP_FILE);
 if (!ip || cap.size() != 16) {
  debugLogA("OFT2 fallback: no route to OSCAR server or invalid file capability");
  f->listener.close();
  return false;
 }
 const int length = WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, nullptr, 0, nullptr, nullptr);
 if (length <= 1 || length > 61) {
  debugLogA("OFT2 fallback: filename cannot be represented in the legacy transfer header");
  f->listener.close();
  return false;
 }
 std::string encoded(size_t(length), '\0');
 if (!WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, &encoded[0], length, nullptr, nullptr)) {
  f->listener.close();
  return false;
 }
 oft2::Rendezvous rv;
 rv.type = 0;
 rv.cookie = f->cookie;
 memcpy(rv.capability.data(), cap.data(), 16);
 rv.externalIp = ip;
 rv.internalIp = Oft2LocalIPv4(T2Utf(m_wszServer));
 rv.port = f->listener.port();
 rv.requestNumber = 1;
 rv.hasOffer = true;
 rv.offer.kind = 1;
 rv.offer.count = 1;
 rv.offer.size = f->size;
 rv.offer.name.assign(encoded.c_str());
 bytes_t payload;
 if (!oft2::build_rendezvous(rv, payload)) {
  f->listener.close();
  debugLogA("OFT2 fallback: failed to encode direct offer");
  return false;
 }
 char receiver[24];
 _itoa(f->uin, receiver, 10);
 bytes_t cookie((const char *)f->cookie.data(), 8);
 uint32_t req;
 { mir_cslock lk(m_csPending); req = m_nextMsgReq++; }
 { mir_cslock lk(m_csFiles); f->offerRequestId = req; }
 Send(OscarPackets::fileRendezvous(receiver, cookie, payload, req));
 ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_LISTENING, f.get());
 if (Oft2IsPrivateIPv4(ip))
  debugLogA("OFT2 DIRECT WARNING: advertised IP %s is not publicly routable; configure Oft2ExternalIP and forward Oft2ListenPort", ars::ip4(ip).c_str());
 debugLogA("OFT2 fallback: direct file offer sent to %u, port=%u, local IP=%s (NAT may block it)",
   (unsigned)f->uin, (unsigned)rv.port, ars::ip4(ip).c_str());
 return true;
}

void CKicqProto::ShutdownOutgoingFiles()
{
 std::vector<std::shared_ptr<OutgoingFile>> files;
 {
  mir_cslock lck(m_csFiles);
  for (auto &it : m_outgoingFiles) files.push_back(it.second);
 }
 for (auto &f : files) {
  InterlockedExchange(&f->cancelled, 1);
  f->listener.close();
  { mir_cslock lk(m_csFiles); if (f->active != INVALID_SOCKET) shutdown(f->active, SD_BOTH); }
 }
 for (auto &f : files) {
  if (f->thread) {
   WaitForSingleObject(f->thread, INFINITE);
   CloseHandle(f->thread);
   f->thread = nullptr;
  }
 }
 std::vector<std::shared_ptr<IncomingFile>> incoming;
 { mir_cslock lck(m_csFiles); for (auto &it : m_incomingFiles) incoming.push_back(it.second); }
 for (auto &f : incoming) {
  InterlockedExchange(&f->cancelled, 1);
  mir_cslock lk(m_csFiles);
  if (f->active != INVALID_SOCKET) shutdown(f->active, SD_BOTH);
 }
 for (auto &f : incoming) if (f->thread) { WaitForSingleObject(f->thread, INFINITE); CloseHandle(f->thread); f->thread = nullptr; }
 mir_cslock lck(m_csFiles);
 m_outgoingFiles.clear();
 m_incomingFiles.clear();
}

bool CKicqProto::FileProgress(OutgoingFile *file, uint64_t done)
{
 if (InterlockedCompareExchange(&file->cancelled, 0, 0)) return false;
 // Avoid flooding Miranda's UI with ~2000 synchronous ACKs per 60 MiB.
 // Always report the first bytes and the exact final byte count.
 const DWORD tick = GetTickCount();
 if (done != file->size && file->lastUiTick &&
     (tick - file->lastUiTick) < 250 &&
     (done - file->lastUiBytes) < (1024ULL * 1024ULL)) return true;
 file->lastUiTick = tick;
 file->lastUiBytes = done;
 PROTOFILETRANSFERSTATUS status = {};
 status.flags = PFTS_SENDING;
 status.totalFiles = 1;
 status.totalBytes = file->size;
 status.totalProgress = done;
 status.currentFileNumber = 0;
 status.currentFileSize = file->size;
 status.currentFileProgress = done;
 ProtoBroadcastAck(file->contact, ACKTYPE_FILE, ACKRESULT_DATA, file, (LPARAM)&status);
 return true;
}

DWORD WINAPI CKicqProto::OutgoingFileThread(LPVOID arg)
{
 std::unique_ptr<OutgoingThreadArgs> ctx((OutgoingThreadArgs *)arg);
 CKicqProto *proto = ctx->proto;
 auto f = ctx->file;

 // ARS relay, with peer-requested redirection (Symnok's receiver-side fallback).
 // All sockets are owned by this worker; callbacks may only call shutdown().
 if (f->viaProxy) {
  bool success = false;
  bool offerSent = false;
  SOCKET sock = INVALID_SOCKET;
  uint32_t ip = 0;
  uint16_t port = 0;
  const std::string cap = OscarText::unhex(CAP_FILE);
  T2Utf proxyHost(proto->m_wszFileProxy);
  const uint16_t configuredProxyPort = (uint16_t)proto->m_iFileProxyPort;
  uint16_t activeProxyPort = configuredProxyPort;
  unsigned allocateReply = 0, allocateError = 0;
  bool allocated = false;
  if (cap.size() == 16 && ars::connectTo(proxyHost, configuredProxyPort, sock)) {
   { mir_cslock lk(proto->m_csFiles); f->active = sock; }
   proto->debugLogA("ARS TX V6: TCP connected to configured relay port %u; ALLOCATE request next", (unsigned)configuredProxyPort);
   if (!InterlockedCompareExchange(&f->cancelled, 0, 0))
    allocated = ars::handshakeAllocate(sock, proto->m_szUin.c_str(), f->cookie, cap,
      ip, port, &allocateReply, &allocateError);
   if (!allocated)
    proto->debugLogA("ARS TX V6: ALLOCATE not accepted on port %u (reply=%u proxyError=%u); checking classic port if applicable",
      (unsigned)configuredProxyPort, allocateReply, allocateError);
  }
  // Classic OSCAR ARS normally listens on TCP/5190. When an account still
  // uses the old 5191 default, retry 5190 only if 5191 did not allocate.
  // Never send a second invitation: this retry occurs before any offer.
  if (!allocated && configuredProxyPort == 5191 && !InterlockedCompareExchange(&f->cancelled, 0, 0) && cap.size() == 16) {
   if (sock != INVALID_SOCKET) {
    { mir_cslock lk(proto->m_csFiles); if (f->active == sock) f->active = INVALID_SOCKET; }
    closesocket(sock); sock = INVALID_SOCKET;
   }
   activeProxyPort = 5190;
   proto->debugLogA("ARS TX V6: retry ALLOCATE on classic ARS TCP port 5190");
   allocateReply = allocateError = 0;
   if (ars::connectTo(proxyHost, activeProxyPort, sock)) {
    { mir_cslock lk(proto->m_csFiles); f->active = sock; }
    allocated = ars::handshakeAllocate(sock, proto->m_szUin.c_str(), f->cookie, cap,
      ip, port, &allocateReply, &allocateError);
    if (!allocated)
     proto->debugLogA("ARS TX V6: classic port did not allocate (reply=%u proxyError=%u)", allocateReply, allocateError);
   }
   else proto->debugLogA("ARS TX V6: classic port 5190 connection failed");
  }
  if (sock != INVALID_SOCKET) {
   if (allocated && !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
    proto->debugLogA("ARS DIAG3: ALLOCATE_OK, advertised relay %s:%u; sending OSCAR offer to %u", ars::ip4(ip).c_str(), (unsigned)port, f->uin);
    oft2::Rendezvous rv;
    rv.type = 0;
    rv.cookie = f->cookie;
    memcpy(rv.capability.data(), cap.data(), 16);
    rv.externalIp = ip;
    // 0x0003 is the sender's local address, NOT the proxy's advertised IP.
    // Some legacy peers use it to distinguish a proxy offer from a direct one.
    rv.internalIp = Oft2LocalIPv4(T2Utf(proto->m_wszServer));
    rv.port = port;
    rv.proxy = true;
    rv.requestNumber = 1;
    rv.hasOffer = true;
    rv.offer.kind = 1;
    rv.offer.count = 1;
    rv.offer.size = f->size;
    const int bytes = WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1 && bytes <= 61) {
     std::string encoded(bytes, '\0');
     WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, &encoded[0], bytes, nullptr, nullptr);
     rv.offer.name = encoded.c_str();
     bytes_t payload;
     // Legacy OSCAR peers may expect proxy TLVs in the conventional order.
     // This serializer is used for outgoing ARS offers ONLY. Incoming and
     // direct transfers keep their existing, tested packet format.
     if (oft2::build_sender_proxy_offer(rv, payload)) {
      char receiver[24]; _itoa(f->uin, receiver, 10);
      bytes_t cookie((const char *)f->cookie.data(), 8);
      uint32_t req;
      { mir_cslock lk(proto->m_csPending); req = proto->m_nextMsgReq++; }
      { mir_cslock lk(proto->m_csFiles); f->offerRequestId = req; }
      proto->Send(OscarPackets::fileRendezvous(receiver, cookie, payload, req));
      offerSent = true;
      proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_LISTENING, f.get());
      proto->debugLogA("ARS TX V5: waiting for relay READY after peer accepts (recipient=%u ticket=%u)",
        (unsigned)f->uin, (unsigned)port);
      unsigned relayResponse = 0;
      bool ready = ars::waitReady(sock, &relayResponse);
      if (!ready)
       proto->debugLogA("ARS TX V5: relay did not become ready, last response=%u (0=socket/timeout; 1=proxy error; other=unexpected)", relayResponse);
      if (!ready && InterlockedCompareExchange(&f->redirected, 0, 0) &&
          !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
       uint32_t redirectIp; uint16_t redirectPort;
       { mir_cslock lk(proto->m_csFiles);
         redirectIp = f->redirectIp; redirectPort = f->redirectPort;
         f->active = INVALID_SOCKET; }
       closesocket(sock); sock = INVALID_SOCKET;
       const std::string host = ars::ip4(redirectIp);
       proto->debugLogA("ARS: peer redirected transfer to %s:%u", host.c_str(), (unsigned)redirectPort);
       if (redirectPort && ars::connectTo(host.c_str(), redirectPort, sock)) {
        { mir_cslock lk(proto->m_csFiles); f->active = sock; }
        ready = ars::handshakeJoin(sock, proto->m_szUin.c_str(), f->cookie, cap, redirectPort);
       }
       if (!ready) {
        if (sock != INVALID_SOCKET) {
         { mir_cslock lk(proto->m_csFiles); f->active = INVALID_SOCKET; }
         closesocket(sock); sock = INVALID_SOCKET;
        }
        // Some servers return a ticket rather than a public TCP port.
        if (ars::connectTo(proxyHost, activeProxyPort, sock)) {
         { mir_cslock lk(proto->m_csFiles); f->active = sock; }
         ready = ars::handshakeJoin(sock, proto->m_szUin.c_str(), f->cookie, cap, redirectPort);
        }
       }
      }
      if (ready && sock != INVALID_SOCKET && !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
       proto->debugLogA("ARS DIAG3: PROXY_READY received, beginning OFT2 PROMPT/ACK/data/DONE");
       FILE *input = nullptr;
       if (_wfopen_s(&input, f->path.c_str(), L"rb") == 0 && input) {
        uint8_t prompt[oft2::HEADER_SIZE] = {};
        memcpy(prompt, "OFT2", 4);
        oft2::put16(prompt + 4, (uint16_t)oft2::HEADER_SIZE);
        oft2::put16(prompt + 6, 0x0101);
        memcpy(prompt + 8, f->cookie.data(), 8);
        oft2::put16(prompt + 20, 1); oft2::put16(prompt + 22, 1);
        oft2::put16(prompt + 24, 1); oft2::put16(prompt + 26, 1);
        oft2::put32(prompt + 28, f->size); oft2::put32(prompt + 32, f->size);
        memcpy(prompt + 192, rv.offer.name.c_str(), rv.offer.name.size());
        proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_CONNECTED, f.get());
        proto->debugLogA("ARS TX: relay paired, sending OFT2 PROMPT, then awaiting ACK/data/DONE");
        // Larger send buffer reduces needless sender stalls on high-latency relays.
        const int socketBuffer = 256 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&socketBuffer, sizeof(socketBuffer));
        int transferStage = oft2::ST_VALIDATE;
        uint64_t transferred = 0;
        const DWORD startedAt = GetTickCount();
        // V8: Mandarin Web can abort large transfers while the sender floods
        // the relay at LAN speed (observed OSCAR CANCEL + WSAECONNRESET).
        // A bounded, optional sender-side rate is used only for >=8 MiB.
        // Preserve the already-working small ARS path byte-for-byte.
        // V9: V8 slept at most 250 ms ONCE per block. If the sender fell
        // behind its pacing schedule, the next block could be sent immediately,
        // so ArsUploadKiBps was not a reliable bandwidth limit.
        // Do not affect the already working <8 MiB transfers.
        const uint32_t configuredKiBps = proto->getDword("ArsUploadKiBps", 512);
        const uint32_t rateKiBps = f->size >= 8U * 1024U * 1024U ? configuredKiBps : 0;
        struct ArsUploadContext {
         OutgoingThreadArgs *args;
         DWORD began;
         uint32_t rate;
        } upload = { ctx.get(), GetTickCount(), rateKiBps };
        proto->debugLogA("ARS TX V9: upload limiter=%u KiB/s, file=%u bytes (0=unlimited)",
          (unsigned)rateKiBps, (unsigned)f->size);
        success = oft2::send_one(sock, prompt, input, f->size,
         [](uint64_t done, uint64_t, void *opaque) -> bool {
          auto *upload = (ArsUploadContext *)opaque;
          auto *args = upload->args;
          if (done >= args->file->nextLogBytes || done == args->file->size) {
           args->proto->debugLogA("ARS TX V9: file data progress %llu/%u bytes", (unsigned long long)done, (unsigned)args->file->size);
           args->file->nextLogBytes = done + 8ULL * 1024ULL * 1024ULL;
          }
          if (!args->proto->FileProgress(args->file.get(), done)) return false;
          if (upload->rate) {
           // Pace the entire transfer, not just the next 250 ms slice.
           // Both the target and multiplication use 64-bit arithmetic;
           // GetTickCount subtraction handles 32-bit timer wraparound.
           const uint64_t targetMs = (done * 1000ULL) / (uint64_t(upload->rate) * 1024ULL);
           while (!InterlockedCompareExchange(&args->file->cancelled, 0, 0)) {
            const DWORD elapsed = GetTickCount() - upload->began;
            if ((uint64_t)elapsed >= targetMs) break;
            const uint64_t remaining = targetMs - elapsed;
            Sleep((DWORD)(remaining > 100 ? 100 : remaining));
           }
          }
          return !InterlockedCompareExchange(&args->file->cancelled, 0, 0);
         }, &upload, &transferStage, &transferred);
        proto->debugLogA("ARS TX V9: OFT2 result=%s stage=%d dataSent=%llu/%u elapsedMs=%llu lastWSA=%d cancelled=%ld (stage 3=data, 4=waiting DONE)",
          success ? "success" : "failure", transferStage,
          (unsigned long long)transferred, (unsigned)f->size,
          (unsigned long long)(DWORD)(GetTickCount()-startedAt), success ? 0 : WSAGetLastError(), (long)InterlockedCompareExchange(&f->cancelled, 0, 0));
        fclose(input);
       }
      }
      else proto->debugLogA("ARS DIAG3 ERROR: no PROXY_READY; relay not paired / peer did not join / redirect failed. Compare sender+receiver logs");
     }
    }
   }
   else proto->debugLogA("ARS DIAG3 ERROR: PROXY_ALLOCATE failed; proxy did not return ALLOCATE_OK");
  }
  else proto->debugLogA("ARS DIAG3 ERROR: proxy TCP connect failed or CAP_FILE not 16 bytes (check proxy hostname, port, firewall)");
  if (sock != INVALID_SOCKET) {
   { mir_cslock lk(proto->m_csFiles); if (f->active == sock) f->active = INVALID_SOCKET; }
   closesocket(sock);
  }
  // Never send a competing direct offer once the ARS offer is visible to the peer.
  // Such a duplicate can leave two pending dialogs and mismatched rendezvous state.
  if (offerSent || success || InterlockedCompareExchange(&f->cancelled, 0, 0)) {
   if (!success) proto->debugLogA("ARS sender: offer was sent but transfer failed; no duplicate direct offer");
   if (InterlockedCompareExchange(&f->terminalAckSent, 1, 0) == 0)
    proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
      success ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
   return 0;
  }
  // The relay never yielded an offer: fall back to direct TCP/OFT2.
  proto->debugLogA("ARS sender: no relay offer sent; attempting direct OFT2 fallback");
  f->viaProxy = false;
  if (!proto->StartDirectFileFallback(f)) {
   if (InterlockedCompareExchange(&f->terminalAckSent, 1, 0) == 0)
    proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_FAILED, f.get(), 0);
   return 0;
  }
 }

 SOCKET sock = f->listener.accept_one(120000, &f->cancelled);
 bool success = false;
 if (sock != INVALID_SOCKET && !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
  DWORD timeout = 30000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
  FILE *input = nullptr;
  if (_wfopen_s(&input, f->path.c_str(), L"rb") == 0 && input) {
   uint8_t prompt[oft2::HEADER_SIZE] = {};
   memcpy(prompt, "OFT2", 4);
   oft2::put16(prompt + 4, (uint16_t)oft2::HEADER_SIZE);
   oft2::put16(prompt + 6, 0x0101);
   memcpy(prompt + 8, f->cookie.data(), 8);
   oft2::put16(prompt + 20, 1);
   oft2::put16(prompt + 22, 1);
   oft2::put16(prompt + 24, 1);
   oft2::put16(prompt + 26, 1);
   oft2::put32(prompt + 28, f->size);
   oft2::put32(prompt + 32, f->size);
   // Legacy OFT2 filenames are typically sent as a NUL-terminated byte string.
   std::string name;
   int required = WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, nullptr, 0, nullptr, nullptr);
   if (required > 0 && required <= 64) {
    name.resize(required);
    WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, &name[0], required, nullptr, nullptr);
    memcpy(prompt + 192, name.data(), name.size());
    const int socketBuffer = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&socketBuffer, sizeof(socketBuffer));
    int stage = oft2::ST_VALIDATE;
    uint64_t sentBytes = 0;
    success = oft2::send_one(sock, prompt, input, f->size, [](uint64_t done, uint64_t, void *ctx) -> bool {
      auto *args = (OutgoingThreadArgs *)ctx;
      return args->proto->FileProgress(args->file.get(), done);
     }, ctx.get(), &stage, &sentBytes);
    proto->debugLogA("OFT2 DIRECT V7: result=%s stage=%d dataSent=%llu/%u WSA=%d",
      success ? "success" : "failure", stage, (unsigned long long)sentBytes,
      (unsigned)f->size, success ? 0 : WSAGetLastError());
   }
   fclose(input);
  }
  closesocket(sock);
 }
 f->listener.close();
 if (!success)
  proto->debugLogA("OFT2 sender: direct transport failed or recipient did not connect");
 if (InterlockedCompareExchange(&f->terminalAckSent, 1, 0) == 0)
  proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
   success ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
 return 0;
}

HANDLE CKicqProto::SendFile(MCONTACT contact, const wchar_t *, wchar_t **files)
{
 if (!m_bOnline || !contact || !files || !files[0] || files[1]) {
  debugLogA("OFT2 sender: rejected before transfer creation (offline, missing contact/file, or multiple files)");
  return nullptr;
 }
 const uint32_t uin = getDword(contact, "UIN");
 if (!uin) { debugLogA("OFT2 sender: missing UIN on selected contact"); return nullptr; }
 // Reject sending a file to this very same OSCAR account. Otherwise servers
 // which echo channel-2 requests can present the offer back as an inbound file.
 if (uin == strtoul(m_szUin.c_str(), nullptr, 10)) {
  debugLogA("OFT2 sender: refused own UIN %u; select a contact on a different account", (unsigned)uin);
  return nullptr;
 }

 auto f = std::make_shared<OutgoingFile>();
 f->contact = contact;
 f->path = files[0];
 size_t slash = f->path.find_last_of(L"/\\");
 f->filename = slash == std::wstring::npos ? f->path : f->path.substr(slash + 1);
 if (f->filename.empty() || f->filename.size() > 60) return nullptr;
 FILE *input = nullptr;
 if (_wfopen_s(&input, f->path.c_str(), L"rb") != 0 || !input) return nullptr;
 _fseeki64(input, 0, SEEK_END);
 const __int64 length = _ftelli64(input);
 fclose(input);
 if (length < 0 || length > UINT32_MAX) return nullptr;
 f->size = (uint32_t)length;
 f->viaProxy = mir_wstrlen(m_wszFileProxy) > 0 && (int)m_iFileProxyPort > 0 && (int)m_iFileProxyPort <= 65535;
 f->uin = uin;
 if (!f->viaProxy && !f->listener.open(INADDR_ANY, getWord("Oft2ListenPort", 0))) return nullptr;
 uint64_t randomCookie = (uint64_t(GetTickCount()) << 16) ^ (uint64_t(rand()) << 32) ^ rand();
 for (int i = 0; i < 8; ++i) f->cookie[i] = uint8_t(randomCookie >> (56 - 8 * i));
 bytes_t payload;
 if (!f->viaProxy) {
 oft2::Rendezvous rv;
 rv.type = 0;
 rv.cookie = f->cookie;
 const bytes_t cap = OscarText::unhex(CAP_FILE);
 if (cap.size() != 16) return nullptr;
 memcpy(rv.capability.data(), cap.data(), 16);
 rv.port = f->listener.port();
 rv.requestNumber = 1;
 const CMStringW configuredPublicIp = getMStringW("Oft2ExternalIP");
 rv.externalIp = Oft2AdvertisedIPv4(configuredPublicIp.c_str(), T2Utf(m_wszServer));
 rv.internalIp = Oft2LocalIPv4(T2Utf(m_wszServer));
 if (!rv.externalIp) return nullptr;
 if (Oft2IsPrivateIPv4(rv.externalIp))
  debugLogA("OFT2 DIRECT WARNING: advertised IP %s is private/unroutable; specify Oft2ExternalIP and forward Oft2ListenPort for Internet clients", ars::ip4(rv.externalIp).c_str());
 rv.hasOffer = true;
 rv.offer.kind = 1;
 rv.offer.count = 1;
 rv.offer.size = f->size;
 // The Rendezvous filename is encoded using the same legacy codepage.
 int n = WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, nullptr, 0, nullptr, nullptr);
 if (n <= 1 || n > 61) return nullptr;
 std::string encoded(n, '\0');
 WideCharToMultiByte(CP_ACP, 0, f->filename.c_str(), -1, &encoded[0], n, nullptr, nullptr);
 rv.offer.name.assign(encoded.c_str());
 if (!oft2::build_rendezvous(rv, payload)) return nullptr;
 }
 auto ctx = new OutgoingThreadArgs{ this, f };
 f->thread = (HANDLE)_beginthreadex(nullptr, 0, (unsigned (__stdcall *)(void *))OutgoingFileThread, ctx, 0, nullptr);
 if (!f->thread) { delete ctx; return nullptr; }
 {
  mir_cslock lck(m_csFiles);
  m_outgoingFiles[f.get()] = f;
 }
 char recipient[20];
 _itoa(uin, recipient, 10);
 bytes_t cookie((char *)f->cookie.data(), 8);
 if (!f->viaProxy) {
  debugLogA("OFT2 sender: sending initial direct OSCAR file offer to %u", (unsigned)uin);
  uint32_t requestId;
  { mir_cslock lk(m_csPending); requestId = m_nextMsgReq++; }
  { mir_cslock lk(m_csFiles); f->offerRequestId = requestId; }
  Send(OscarPackets::fileRendezvous(recipient, cookie, payload, requestId));
  ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_LISTENING, f.get());
 }
 else debugLogA("ARS sender: starting relay allocation for %u (offer not yet sent)", (unsigned)uin);
 return f.get();
}

// Direct OFT2 receive path. The peer's address is never trusted as a filename.
static bool Oft2IncomingProgress(uint64_t done, uint64_t, void *context)
{
 auto *arg = (CKicqProto::IncomingThreadArgs *)context;
 return arg->proto->IncomingProgress(arg->file.get(), done);
}

bool CKicqProto::IncomingProgress(IncomingFile *f, uint64_t done)
{
 if (InterlockedCompareExchange(&f->cancelled, 0, 0)) return false;
 PROTOFILETRANSFERSTATUS status = {};
 // The standard Miranda transfer window uses these paths when the user
 // clicks "Transfer complete - open file".  Without them it falls back to
 // the receive directory (typically Documents).
 // The path is finalised in FileAllow before the receiving worker starts.
 const size_t separator = f->destination.find_last_of(L"\\/");
 const std::wstring workingDir = separator == std::wstring::npos
  ? std::wstring() : f->destination.substr(0, separator + 1);
 status.flags = PFTS_UNICODE | PFTS_RECEIVING;
 status.hContact = f->contact;
 status.totalFiles = 1;
 status.currentFileNumber = 0;
 status.totalBytes = f->size;
 status.totalProgress = done;
 status.currentFileSize = f->size;
 status.currentFileProgress = done;
 status.szWorkingDir.w = const_cast<wchar_t *>(workingDir.c_str());
 status.szCurrentFile.w = const_cast<wchar_t *>(f->destination.c_str());
 ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_DATA, f, (LPARAM)&status);
 return true;
}

DWORD WINAPI CKicqProto::IncomingFileThread(LPVOID param)
{
 std::unique_ptr<IncomingThreadArgs> arg((IncomingThreadArgs *)param);
 auto f = arg->file;
 bool ok = false;

 // Peer-advertised ARS relay. If unreachable, allocate our own and redirect.
 if (f->viaProxy) {
  SOCKET sock = INVALID_SOCKET;
  const std::string cap = OscarText::unhex(CAP_FILE);
  const std::string host = ars::ip4(f->ip);
  bool joined = false;
  // In a proxied channel-2 offer TLV(0x0005) is an ARS session/ticket
  // check value, NOT the TCP listening port. Passing it to connectTo()
  // leaves the receiver at 0% even though the invitation was accepted.
  // Keep f->port for handshakeJoin() only. Some deployments (Mandarin)
  // use a configured alternate proxy port; classic OSCAR uses TCP 5190.
  const uint16_t configuredProxyPort =
    (uint16_t)arg->proto->m_iFileProxyPort;
  const uint16_t proxyPorts[] = { configuredProxyPort, 5190 };
  for (unsigned portIndex = 0; portIndex < 2 && !joined; ++portIndex) {
   const uint16_t tcpPort = proxyPorts[portIndex];
   if (!tcpPort || (portIndex && tcpPort == proxyPorts[0])) continue;
   arg->proto->debugLogA("ARS RX: connecting advertised proxy %s TCP:%u (ticket=%u)",
     host.c_str(), (unsigned)tcpPort, (unsigned)f->port);
   if (!cap.empty() && ars::connectTo(host.c_str(), tcpPort, sock)) {
    { mir_cslock lk(arg->proto->m_csFiles); f->active = sock; }
    joined = ars::handshakeJoin(sock, arg->proto->m_szUin.c_str(), f->cookie, cap, f->port);
    arg->proto->debugLogA("ARS RX: advertised relay JOIN %s (TCP:%u)",
      joined ? "READY" : "failed", (unsigned)tcpPort);
    if (!joined) {
     { mir_cslock lk(arg->proto->m_csFiles); f->active = INVALID_SOCKET; }
     closesocket(sock); sock = INVALID_SOCKET;
    }
   }
  }
  T2Utf proxyHost(arg->proto->m_wszFileProxy);
  // If the advertised relay address cannot be reached, try the configured
  // hostname. Use the configured TCP port, never the invitation ticket.
  if (!joined && cap.size() == 16 && configuredProxyPort &&
      ars::connectTo(proxyHost, configuredProxyPort, sock)) {
   { mir_cslock lk(arg->proto->m_csFiles); f->active = sock; }
   joined = ars::handshakeJoin(sock, arg->proto->m_szUin.c_str(), f->cookie, cap, f->port);
   arg->proto->debugLogA("ARS RX: configured relay JOIN %s (TCP:%u ticket=%u)",
     joined ? "READY" : "failed", (unsigned)configuredProxyPort, (unsigned)f->port);
   if (!joined) {
    { mir_cslock lk(arg->proto->m_csFiles); f->active = INVALID_SOCKET; }
    closesocket(sock); sock = INVALID_SOCKET;
   }
  }
  if (!joined && cap.size() == 16 && !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
   // Two distinct ARS servers / nonpublic relay ticket: invite the sender to ours.
   uint32_t ip = 0; uint16_t port = 0;
   if (ars::connectTo(proxyHost, (uint16_t)arg->proto->m_iFileProxyPort, sock)) {
    { mir_cslock lk(arg->proto->m_csFiles); f->active = sock; }
    if (ars::handshakeAllocate(sock, arg->proto->m_szUin.c_str(), f->cookie, cap, ip, port)) {
     oft2::Rendezvous rv;
     rv.type = 0; rv.cookie = f->cookie;
     memcpy(rv.capability.data(), cap.data(), 16);
     rv.proxy = true; rv.requestNumber = 2; rv.externalIp = ip;
     rv.port = port;
     bytes_t payload;
     if (oft2::build_rendezvous(rv, payload)) {
      char receiver[24]; _itoa(f->senderUin, receiver, 10);
      bytes_t cookie((const char*)f->cookie.data(), 8);
      arg->proto->debugLogA("ARS: original proxy unreachable; sending relay redirect");
      arg->proto->Send(OscarPackets::fileRendezvous(receiver, cookie, payload, 6));
      joined = ars::waitReady(sock);
     }
    }
   }
  }
  if (joined && !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
   char receiver[24]; _itoa(f->senderUin, receiver, 10);
   oft2::Rendezvous accept;
   accept.type = 2; accept.cookie = f->cookie;
   memcpy(accept.capability.data(), cap.data(), 16);
   bytes_t payload, cookie((const char*)f->cookie.data(), 8);
   if (oft2::build_rendezvous(accept, payload))
    arg->proto->Send(OscarPackets::fileRendezvous(receiver, cookie, payload, 6));
   arg->proto->debugLogA("ARS DIAG3: receiver got READY; sending accept and waiting for OFT2 PROMPT");
   arg->proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_CONNECTED, f.get());
   FILE *output = nullptr;
   if (_wfopen_s(&output, f->destination.c_str(), L"wb") == 0 && output) {
    arg->proto->debugLogA("ARS RX: TCP paired, waiting for OFT2 PROMPT (size=%u)", (unsigned)f->size);
    ok = oft2::receive_one(sock, output, f->size, Oft2IncomingProgress, arg.get());
    arg->proto->debugLogA("ARS RX: OFT2 transfer %s", ok ? "completed" : "failed during PROMPT/ACK/data/DONE");
    if (fclose(output)) ok = false;
    if (!ok) _wremove(f->destination.c_str());
   }
  }
  else arg->proto->debugLogA("ARS DIAG3 ERROR: receiver could not JOIN source or fallback proxy; check source relay and proxy reachability");
  if (sock != INVALID_SOCKET) {
   { mir_cslock lk(arg->proto->m_csFiles); if (f->active == sock) f->active = INVALID_SOCKET; }
   closesocket(sock);
  }
  if (ok) arg->proto->IncomingProgress(f.get(), f->size);
  arg->proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
   ok ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
  return 0;
 }

 SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
 if (sock != INVALID_SOCKET) {
  DWORD timeout = 30000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(f->port);
  addr.sin_addr.s_addr = htonl(f->ip);
  if (connect(sock, (sockaddr *)&addr, sizeof(addr)) == 0 &&
      !InterlockedCompareExchange(&f->cancelled, 0, 0)) {
   FILE *output = nullptr;
   if (_wfopen_s(&output, f->destination.c_str(), L"wb") == 0 && output) {
    ok = oft2::receive_one(sock, output, f->size, Oft2IncomingProgress, arg.get());
    if (fclose(output)) ok = false;
    if (!ok) _wremove(f->destination.c_str());
   }
  }
  closesocket(sock);
 }
 if (ok) arg->proto->IncomingProgress(f.get(), f->size);
 arg->proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
   ok ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
 return 0;
}

// OFT2 transfer only: never trust a filename supplied by a remote peer.
// Reject paths instead of silently stripping them (prevents traversal/ADS).
static bool Oft2SafeIncomingName(const std::wstring &name)
{
 if (name.empty() || name.size() > 240 || name == L"." || name == L"..") return false;
 if (name.back() == L'.' || name.back() == L' ') return false;
 if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
 for (wchar_t c : name) if (c < 32) return false;
 std::wstring stem = name.substr(0, name.find(L'.'));
 for (auto reserved : { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" })
  if (!_wcsicmp(stem.c_str(), reserved)) return false;
 return true;
}

// Called on BOS thread. Never close a worker socket here: shutdown only wakes
// its blocking read; the transfer worker owns close and reconnect.
void CKicqProto::HandleFileRedirect(const oft2::Rendezvous &rv)
{
 if (!rv.proxy || rv.type != 0 || rv.requestNumber != 2 || !rv.port || !rv.externalIp)
  return;
 mir_cslock lock(m_csFiles);
 for (auto &entry : m_outgoingFiles) {
  auto &f = entry.second;
  if (!f->viaProxy || memcmp(f->cookie.data(), rv.cookie.data(), 8) != 0 ||
      InterlockedCompareExchange(&f->cancelled, 0, 0)) continue;
  f->redirectIp = rv.externalIp;
  f->redirectPort = rv.port;
  InterlockedExchange(&f->redirected, 1);
  debugLogA("ARS: received proxy redirect from peer");
  if (f->active != INVALID_SOCKET) shutdown(f->active, SD_BOTH);
  break;
 }
}

void CKicqProto::HandleFileOffer(MCONTACT contact, const oft2::Rendezvous &rv)
{
 debugLogA("ARS DIAG3: incoming OSCAR file offer, proxy=%u, relay=%s:%u, count=%u", (unsigned)rv.proxy, ars::ip4(rv.externalIp).c_str(), (unsigned)rv.port, (unsigned)rv.offer.count);
 if (!contact || !rv.hasOffer || rv.type != 0 || rv.offer.count != 1 ||
     !rv.port || (!rv.externalIp && !rv.internalIp)) {
  debugLogA("ARS DIAG3 ERROR: incoming offer rejected before UI: contact=%u info=%d type=%u count=%u port=%u ip=%u", contact != 0, rv.hasOffer, (unsigned)rv.type, (unsigned)rv.offer.count, (unsigned)rv.port, (unsigned)rv.externalIp);
  return;
 }
 auto f = std::make_shared<IncomingFile>();
 f->contact = contact;
 f->size = rv.offer.size;
 f->viaProxy = rv.proxy;
 f->senderUin = getDword(contact, "UIN");
 f->port = rv.port;
 f->ip = rv.externalIp ? rv.externalIp : rv.internalIp;
 f->cookie = rv.cookie;
 const int count = MultiByteToWideChar(CP_ACP, 0, rv.offer.name.c_str(), -1, nullptr, 0);
 if (count < 2 || count > 256) return;
 std::wstring name(count, L'\0');
 MultiByteToWideChar(CP_ACP, 0, rv.offer.name.c_str(), -1, &name[0], count);
 name.resize(count - 1);
 if (!Oft2SafeIncomingName(name)) {
  debugLogA("OFT2: refused unsafe remote filename");
  return;
 }
 f->filename = name;
 {
  mir_cslock lock(m_csFiles);
  m_incomingFiles[f.get()] = f;
 }
 DB::FILE_BLOB blob(f->filename.c_str());
 blob.setSize(f->size);
 blob.setUserInfo(f.get());
 DB::EventInfo dbei;
 // SQLite needs a valid protocol module for incoming file events.
 // Without it, db_event_add can fail with SQLITE_CONSTRAINT (19),
 // leaving the incoming request invisible in Miranda.
 dbei.szModule = m_szModuleName;
 dbei.eventType = EVENTTYPE_FILE;
 dbei.iTimestamp = time(nullptr);
 const INT_PTR hFileEvent = ProtoChainRecvFile(contact, blob, dbei);
 debugLogA("ARS RX FIX5: ProtoChainRecvFile result=%llu, contact=%llu, module=%s",
  (unsigned long long)hFileEvent, (unsigned long long)contact, m_szModuleName);
}

HANDLE CKicqProto::FileAllow(MCONTACT, HANDLE transfer, const wchar_t *path)
{
 debugLogA("ARS DIAG3: user accepted incoming file offer; starting relay JOIN");
 std::shared_ptr<IncomingFile> f;
 { mir_cslock lock(m_csFiles); auto it = m_incomingFiles.find(transfer);
   if (it == m_incomingFiles.end()) return nullptr;
   f = it->second; }
 if (!path || !*path) return nullptr;
 {
  mir_cslock lock(m_csFiles);
  auto it = m_incomingFiles.find(transfer);
  if (it == m_incomingFiles.end() || it->second != f || f->thread) return nullptr;
  f->destination = path;
  if (f->destination.back() != L'\\' && f->destination.back() != L'/') f->destination += L'\\';
  f->destination += f->filename;
  // Never overwrite an existing file as a side effect of accepting an offer.
  if (GetFileAttributesW(f->destination.c_str()) != INVALID_FILE_ATTRIBUTES) return nullptr;
  auto *args = new IncomingThreadArgs{ this, f };
  f->thread = (HANDLE)_beginthreadex(nullptr, 0, (unsigned (__stdcall *)(void *))IncomingFileThread, args, 0, nullptr);
  if (!f->thread) { delete args; return nullptr; }
 }
 return f.get();
}

int CKicqProto::FileDeny(MCONTACT, HANDLE transfer, const wchar_t *)
{
 mir_cslock lock(m_csFiles);
 auto it = m_incomingFiles.find(transfer);
 if (it == m_incomingFiles.end()) return 1;
 if (it->second->thread) return 1;
 m_incomingFiles.erase(it);
 return 0;
}

int CKicqProto::FileCancel(MCONTACT, HANDLE transfer)
{
 mir_cslock lock(m_csFiles);
 auto it = m_outgoingFiles.find(transfer);
 if (it != m_outgoingFiles.end()) {
  InterlockedExchange(&it->second->cancelled, 1);
  if (it->second->active != INVALID_SOCKET) shutdown(it->second->active, SD_BOTH);
  return 0;
 }
 auto incoming = m_incomingFiles.find(transfer);
 if (incoming == m_incomingFiles.end()) return 1;
 InterlockedExchange(&incoming->second->cancelled, 1);
 if (incoming->second->active != INVALID_SOCKET) shutdown(incoming->second->active, SD_BOTH);
 return 0;
}

int CKicqProto::FileResume(HANDLE, int, const wchar_t *)
{
 return 1;
}

MEVENT CKicqProto::RecvFile(MCONTACT hContact, DB::FILE_BLOB &blob, DB::EventInfo &dbei)
{
 // Use Miranda's native file-receive pipeline. It creates the unread
 // EVENTTYPE_FILE database record AND a CLISTEVENT with service
 // SRFile/RecvFile, which opens the standard Accept/Decline dialog.
 // Writing with db_event_add() directly (FIX5-7) bypassed that UI path.
 dbei.szModule = m_szModuleName;
 dbei.eventType = EVENTTYPE_FILE;
 dbei.flags &= ~(DBEF_READ | DBEF_SENT | DBEF_TEMPORARY);
 const MEVENT hEvent = CSuper::RecvFile(hContact, blob, dbei);
 debugLogA("ARS RX FIX8: native Miranda file receive event=%llu contact=%llu",
  (unsigned long long)hEvent, (unsigned long long)hContact);
 return hEvent;
}

// Use Windows CryptoAPI for the required 16-byte OSCAR icon hash.
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
static bool CalcAvatarMd5(const bytes_t &data, bytes_t &digest)
{
 HCRYPTPROV provider = 0;
 HCRYPTHASH hash = 0;
 BYTE value[16] = {};
 DWORD length = sizeof(value);
 bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) != FALSE;
 if (ok) ok = CryptCreateHash(provider, CALG_MD5, 0, 0, &hash) != FALSE;
 if (ok) ok = CryptHashData(hash, reinterpret_cast<const BYTE *>(data.data()), DWORD(data.size()), 0) != FALSE;
 if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, value, &length, 0) != FALSE && length == 16;
 if (hash) CryptDestroyHash(hash);
 if (provider) CryptReleaseContext(provider, 0);
 if (ok) digest.assign(reinterpret_cast<char *>(value), 16);
 return ok;
}

// A cache entry is usable only when it is an actual non-empty file.
static bool KicqAvatarFileReady(const wchar_t *path)
{
 if (!path || !*path) return false;
 WIN32_FILE_ATTRIBUTE_DATA attributes = {};
 if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) return false;
 return !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
  (attributes.nFileSizeHigh != 0 || attributes.nFileSizeLow != 0);
}

// Decode only protocol-provided hex hashes. A missing disk cache can be fetched
// again even if the server does not send another presence update.
static bool KicqAvatarDecodeHash(const char *hex, bytes_t &raw)
{
 if (!hex) return false;
 const size_t length = strlen(hex);
 if (!length || length > 128 || (length & 1)) return false;
 raw.clear();
 for (size_t i = 0; i < length; i += 2) {
  auto digit = [](char value) -> int {
   if (value >= '0' && value <= '9') return value - '0';
   if (value >= 'a' && value <= 'f') return value - 'a' + 10;
   if (value >= 'A' && value <= 'F') return value - 'A' + 10;
   return -1;
  };
  const int hi = digit(hex[i]), lo = digit(hex[i + 1]);
  if (hi < 0 || lo < 0) { raw.clear(); return false; }
  raw.push_back(char((hi << 4) | lo));
 }
 return !raw.empty();
}

// Use the same cache validation from proactive discovery and AVS requests.
void CKicqProto::QueueCachedBartAvatar(MCONTACT contact)
{
 if (!contact) return;
 auto announced = getMStringA(contact, "BartAdvertisedHash");
 const auto cached = getMStringA(contact, "BartAvatarHash");
 const auto file = getMStringW(contact, "BartAvatarPath");
 if (announced.IsEmpty()) announced = cached; // older XP profiles saved only the verified hash
 if (announced == cached && KicqAvatarFileReady(file.c_str())) return;
 bytes_t raw;
 if (KicqAvatarDecodeHash(announced.c_str(), raw))
  QueueBartAvatar(ContactId(contact), db_get_w(contact, m_szModuleName, "BartAdvertisedType", 1), raw);
}

// Avatar Services (Miranda NG 0.96.x). Uses existing BART wire/session.
// Called by Miranda Avatar Service to obtain an actual cached contact image.
// An advertised hash alone is never reported as a successfully downloaded image.
INT_PTR MIR_CDECL CKicqProto::OnGetAvatarInfo(WPARAM, LPARAM lParam)
{
 auto *info = reinterpret_cast<PROTO_AVATAR_INFORMATION *>(lParam);
 if (!info || !info->hContact) return GAIR_NOAVATAR;
 const auto file = getMStringW(info->hContact, "BartAvatarPath");
 const auto advertisedHash = getMStringA(info->hContact, "BartAdvertisedHash");
 const auto cachedHash = getMStringA(info->hContact, "BartAvatarHash");
 if ((advertisedHash.IsEmpty() || advertisedHash == cachedHash) && KicqAvatarFileReady(file.c_str())) {
  const wchar_t *ext = wcsrchr(file.c_str(), L'.');
  info->format = PA_FORMAT_UNKNOWN;
  if (ext) {
   if (!_wcsicmp(ext, L".png")) info->format = PA_FORMAT_PNG;
   else if (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg")) info->format = PA_FORMAT_JPEG;
   else if (!_wcsicmp(ext, L".gif")) info->format = PA_FORMAT_GIF;
   else if (!_wcsicmp(ext, L".bmp")) info->format = PA_FORMAT_BMP;
  }
  if (file.GetLength() >= MAX_PATH) return GAIR_NOAVATAR;
  wcscpy_s(info->filename, _countof(info->filename), file.c_str());
  return GAIR_SUCCESS;
 }
 // XP recovery: cached BART files live in the temporary directory and may
 // disappear between sessions. Re-request the last verified avatar hash,
 // without forcing a contact-list/Extra Icons rebuild.
 if (!m_bOnline) return GAIR_NOAVATAR;
 const auto peer = ContactId(info->hContact);
 QueueCachedBartAvatar(info->hContact);
 // Unknown hash: schedule discovery once per session, not on every AVS call.
 ScheduleBartRefresh(peer);
 mir_cslock lock(m_csBart);
 for (const auto &pending : m_bartPending)
  if (pending.screenName == peer) return GAIR_WAITFOR;
 return GAIR_NOAVATAR;
}

INT_PTR MIR_CDECL CKicqProto::OnAvatarCaps(WPARAM wParam, LPARAM lParam)
{
 switch (wParam) {
 case AF_ENABLED: return 1;
 case AF_MAXSIZE: if (lParam) { auto *size = reinterpret_cast<POINT *>(lParam); size->x = 128; size->y = 128; } return 0;
 case AF_PROPORTION: return PIP_SQUARE;
 case AF_MAXFILESIZE: return 7168;
 case AF_FORMATSUPPORTED: return 1; // Image validation occurs before upload.
 case AF_FETCHIFCONTACTOFFLINE: return 1;
 }
 return 0;
}

INT_PTR MIR_CDECL CKicqProto::OnGetMyAvatar(WPARAM wParam, LPARAM lParam)
{
 wchar_t *dest = reinterpret_cast<wchar_t *>(wParam);
 size_t count = static_cast<size_t>(lParam);
 if (!dest || !count || count > 32768) return -1;
 auto path = getMStringW("BartOwnAvatarPath");
 if (path.IsEmpty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return -1;
 if (path.GetLength() >= (int)count) return -1;
 wcscpy_s(dest, count, path.c_str());
 return 0;
}

INT_PTR MIR_CDECL CKicqProto::OnSetMyAvatar(WPARAM, LPARAM lParam)
{
 if (!m_bOnline) return -1;
 const wchar_t *path = reinterpret_cast<const wchar_t *>(lParam);
 const bool deleting = path == nullptr || !*path;
 bytes_t data, hash;
 if (!deleting) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return -1;
  LARGE_INTEGER length = {};
  bool valid = GetFileSizeEx(file, &length) && length.QuadPart >= 6 && length.QuadPart <= 7168;
  if (valid) {
   data.resize(size_t(length.QuadPart)); DWORD read = 0;
   valid = ReadFile(file, &data[0], DWORD(data.size()), &read, nullptr) && read == data.size();
  }
  CloseHandle(file);
  if (!valid || !bart::isSupportedImage(data) || !CalcAvatarMd5(data, hash)) return -1;
 }
 uint16_t itemId = m_ownIconItemId;
 if (!itemId) itemId = getWord("BartOwnIconItemId", 0);
 if (!itemId && deleting) return 0; // Already absent.
 if (!itemId) itemId = 0x4000; // Candidate item ID; server may reject a collision.
 const bool existingIcon = m_ownIconItemId || getWord("BartOwnIconItemId", 0);
 bytes_t record;
 if (deleting) {
  if (!bartssi::deleteItem(itemId, record)) return -1;
 } else if (!bartssi::iconItem(itemId, hash, record)) return -1;
 uint32_t request;
 {
  mir_cslock lock(m_csBart);
  if (m_ownIconSsiRequest) return -1; // Avoid overlapping edits.
  request = ++m_ownIconRequestCounter;
  m_ownIconSsiRequest = request;
  m_ownIconDeleting = deleting;
  m_ownIconPendingHash = hash;
  m_ownIconPendingUpload = data;
  m_ownIconPendingPath = deleting ? L"" : path;
 }
 Send(OscarPackets::ssiEditStart());
 Send(OscarSnac(0x13, deleting ? 0x0A : (existingIcon ? 0x09 : 0x08), request, record));
 Send(OscarPackets::ssiEditEnd());
 // Accepted locally. The server's SSI result, then BART's upload result decide success.
 return 0;
}

INT_PTR __cdecl CKicqProto::OnRetryAuth(WPARAM w, LPARAM)
{
 MCONTACT c=(MCONTACT)w;
 if(!c || mir_strcmp(Proto_GetBaseAccountName(c),m_szModuleName) || !getByte(c,"Auth",0)) return 1;
 return AuthRequest(c,TranslateT("Please authorize me"));
}
int __cdecl CKicqProto::OnPreBuildContactMenu(WPARAM w, LPARAM)
{
 MCONTACT c=(MCONTACT)w;
 if(m_hRetryAuthMenu) Menu_ShowItem(m_hRetryAuthMenu,c && m_bOnline && !mir_strcmp(Proto_GetBaseAccountName(c),m_szModuleName) && getByte(c,"Auth",0));
 return 0;
}
