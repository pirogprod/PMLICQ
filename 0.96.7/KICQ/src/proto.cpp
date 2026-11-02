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
#include "xstatus_catalog.h"
#include "oft2_socket_transfer.h"
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
 // Do not show a cached X-status for an offline contact.
 const WORD presence = db_get_w(contact, module, "Status", ID_STATUS_OFFLINE);
 if (presence == ID_STATUS_OFFLINE) {
  ExtraIcon_SetIcon(g_kicqExtraIcon, contact, INVALID_HANDLE_VALUE);
  return 0;
 }

 // Only display an X-status explicitly confirmed by the current OSCAR session.
 // A stale XStatusId alone must never allocate an extra-icon slot.
 const int confirmed = db_get_b(contact, module, "XStatusLive", 0);
 int id = confirmed ? db_get_b(contact, module, "XStatusId", 0) : 0;
 if (id >= 1 && id <= 35) {
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

static int CompareUsers(const KicqUser *p1, const KicqUser *p2)
{
	return (p1->uin < p2->uin) ? -1 : (p1->uin > p2->uin) ? 1 : 0;
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
	m_wszDefaultGroup(this, "DefaultGroup", L"General")
{
	NETLIBUSER nlu = {};
	nlu.flags = NUF_OUTGOING | NUF_UNICODE;
	nlu.szSettingsModule = m_szModuleName;
	nlu.szDescriptiveName.w = m_tszUserName;
	m_hNetlibUser = Netlib_RegisterUser(&nlu);

	HookProtoEvent(ME_OPT_INITIALISE, &CKicqProto::OnOptionsInit);
	CreateProtoService("/XStatusEditor", &CKicqProto::OnMenuXStatus);
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
 StopBart();
 ShutdownOutgoingFiles();
}

void CKicqProto::OnModulesLoaded()
{
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

MCONTACT CKicqProto::FindContact(uint32_t uin)
{
	for (auto &hContact : AccContacts())
		if (getDword(hContact, "UIN") == uin)
			return hContact;
	return 0;
}

MCONTACT CKicqProto::CreateContact(uint32_t uin, bool bTemporary)
{
	MCONTACT hContact = FindContact(uin);
	if (hContact)
		return hContact;

	hContact = db_add_contact();
	if (!hContact)
		return 0;
	Proto_AddToContact(hContact, m_szModuleName);
	if (bTemporary)
		db_set_b(hContact, "CList", "NotOnList", 1);
	setDword(hContact, "UIN", uin);
	setWord(hContact, "Status", ID_STATUS_OFFLINE);
	return hContact;
}

KicqUser* CKicqProto::FindUser(uint32_t uin)
{
	KicqUser tmp(uin, 0);
	return m_arUsers.find(&tmp);
}

KicqUser* CKicqProto::AddUser(uint32_t uin, MCONTACT hContact)
{
	if (auto *pUser = FindUser(uin))
		return pUser;

	auto *pUser = new KicqUser(uin, hContact);
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

	if (getDword("UIN") == 0 || getMStringA("Password").IsEmpty()) {
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
		return PF1_IM | PF1_SERVERCLIST | PF1_AUTHREQ | PF1_BASICSEARCH | PF1_ADDSEARCHRES | PF1_MODEMSGRECV | PF1_MODEMSGSEND | PF1_NUMERICUSERID;

	case PFLAGNUM_2:
		return PF2_ONLINE | PF2_SHORTAWAY | PF2_LONGAWAY | PF2_LIGHTDND | PF2_HEAVYDND | PF2_FREECHAT | PF2_INVISIBLE;

	case PFLAGNUM_3:
		return PF2_SHORTAWAY | PF2_LONGAWAY | PF2_LIGHTDND | PF2_HEAVYDND | PF2_FREECHAT;

	case PFLAGNUM_4:
		return PF4_SUPPORTTYPING | PF4_IMSENDOFFLINE | PF4_AVATARS;

	case PFLAG_UNIQUEIDTEXT:
		return (INT_PTR)TranslateT("UIN");

	case PFLAG_MAXLENOFMESSAGE:
		return 2048;
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// messaging

int CKicqProto::SendMsg(MCONTACT hContact, MEVENT, const char *pszSrc)
{
	uint32_t uin = getDword(hContact, "UIN");
	int seq = InterlockedIncrement((long *)&m_msgSeq);
	if (!m_bOnline || uin == 0) {
		ProtoBroadcastAsync(hContact, ACKTYPE_MESSAGE, ACKRESULT_FAILED, (HANDLE)seq, (LPARAM)TranslateT("You are offline"));
		return seq;
	}

	CMStringW text(Utf2T(pszSrc).get());
	text.Replace(L"\n", L"\r\n");

	char szUin[20];
	_itoa(uin, szUin, 10);

	bytes_t cookie(8, 0);
	uint64_t id = GetTickCount() ^ (uint64_t(rand()) << 32);
	for (int i = 0; i < 8; i++)
		cookie[i] = char(id >> (8 * (7 - i)));

	bool bRelay = false, bUtf8 = false, bOnline = false;
	{
		mir_cslock lck(m_csRoster);
		if (auto *pUser = FindUser(uin)) {
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
		m_pendingMsgs[reqId] = { uin, seq };
	}

	if (bRelay)
		Send(OscarPackets::messageChannel2(szUin, text, bUtf8, cookie, reqId));
	else if (!bOnline) {
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
	uint32_t uin = getDword(hContact, "UIN");
	if (!m_bOnline || uin == 0 || m_iStatus == ID_STATUS_INVISIBLE)
		return 0;

	char szUin[20];
	_itoa(uin, szUin, 10);
	Send(OscarPackets::typingNotify(szUin, type == PROTOTYPE_SELFTYPING_ON ? 2 : 0));
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// roster edits

void CKicqProto::SsiAddContact(MCONTACT hContact, int groupId)
{
	uint32_t uin = getDword(hContact, "UIN");
	char szUin[20];
	_itoa(uin, szUin, 10);

	CMStringW wszNick(db_get_wsm(hContact, "CList", "MyHandle"));
	if (wszNick.IsEmpty())
		wszNick = getMStringW(hContact, "Nick");
	if (wszNick.IsEmpty())
		wszNick = _A2T(szUin).get();

	m_pendAdd.uin = uin;
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
	uint32_t uin = (psr->flags & PSR_UNICODE) ? _wtoi(psr->id.w) : atoi(psr->id.a);
	if (uin == 0)
		return 0;

	bool bTemporary = (flags & PALF_TEMPORARY) != 0;
	MCONTACT hContact = CreateContact(uin, bTemporary);
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

	CMStringW wszUin(FORMAT, L"%u", getDword(hContact, "UIN"));
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

	uint32_t uin = getDword(hContact, "UIN");
	int groupId = 0, ssiId = 0;
	{
		mir_cslock lck(m_csRoster);
		if (auto *pUser = FindUser(uin)) {
			groupId = pUser->groupId;
			ssiId = pUser->ssiId;
			m_arUsers.remove(pUser);
		}
	}

	if (ssiId != 0) {
		char szUin[20];
		_itoa(uin, szUin, 10);
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

	uint32_t uin = getDword(hContact, "UIN");
	CMStringW wszNick(db_get_wsm(hContact, "CList", "MyHandle"));
	if (wszNick.IsEmpty())
		wszNick.Format(L"%u", uin);

	int groupId = 0, ssiId = 0;
	bool bAuthorized = true;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(uin);
		if (pUser == nullptr || pUser->ssiId == 0)
			return 0;
		groupId = pUser->groupId;
		ssiId = pUser->ssiId;
		bAuthorized = pUser->bAuthorized;
	}

	if (getMStringW(hContact, "SrvNick") == wszNick)
		return 0; // the change came from the server

	char szUin[20];
	_itoa(uin, szUin, 10);
	m_pendRename.uin = uin;
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

	uint32_t uin = getDword(hContact, "UIN");
	int oldGroup = 0, ssiId = 0, newGroup = 0;
	{
		mir_cslock lck(m_csRoster);
		auto *pUser = FindUser(uin);
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
	char szUin[20];
	_itoa(uin, szUin, 10);
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
	uint32_t uin = getDword(hContact, "UIN");
	if (!m_bOnline || uin == 0)
		return 1;

	char szUin[20];
	_itoa(uin, szUin, 10);
	Send(OscarPackets::futureAuthGrant(szUin));
	Send(OscarPackets::authRequest(szUin, mir_wstrlen(szMessage) ? szMessage : TranslateT("Please authorize me")));
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
	uint32_t uin = hContact ? getDword(hContact, "UIN") : 0;
	if (!m_bOnline || uin == 0)
		return 1;

	char szUin[20];
	_itoa(uin, szUin, 10);
	Send(OscarPackets::authReply(szUin, true));
	return 0;
}

int CKicqProto::AuthDeny(MEVENT hDbEvent, const wchar_t *)
{
	MCONTACT hContact = AuthEventContact(m_szModuleName, hDbEvent);
	uint32_t uin = hContact ? getDword(hContact, "UIN") : 0;
	if (!m_bOnline || uin == 0)
		return 1;

	char szUin[20];
	_itoa(uin, szUin, 10);
	Send(OscarPackets::authReply(szUin, false));
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
	if (!m_bOnline || uin == 0)
		return 1;

	RequestUserInfo(uin, hContact, nullptr);
	return 0;
}

HANDLE CKicqProto::SearchBasic(const wchar_t *id)
{
	uint32_t uin = _wtoi(id);
	if (!m_bOnline || uin == 0)
		return nullptr;

	static long searchId = 0;
	HANDLE hSearch = (HANDLE)(INT_PTR)InterlockedIncrement(&searchId);
	RequestUserInfo(uin, 0, hSearch);
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
 Send(OscarPackets::setUserInfo(1, 0, 0, guid));
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
static uint32_t Oft2LocalIPv4()
{
 SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 if (s == INVALID_SOCKET) return 0;
 sockaddr_in peer = {};
 peer.sin_family = AF_INET;
 peer.sin_port = htons(5190);
 peer.sin_addr.s_addr = inet_addr(KICQ_DEFAULT_SERVER);
 uint32_t ip = 0;
 if (peer.sin_addr.s_addr != INADDR_NONE &&
     connect(s, (const sockaddr *)&peer, sizeof(peer)) != SOCKET_ERROR) {
  sockaddr_in local = {};
  int len = sizeof(local);
  if (getsockname(s, (sockaddr *)&local, &len) == 0)
   ip = ntohl(local.sin_addr.s_addr);
 }
 closesocket(s);
 return ip;
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
 for (auto &f : incoming) InterlockedExchange(&f->cancelled, 1);
 for (auto &f : incoming) if (f->thread) { WaitForSingleObject(f->thread, INFINITE); CloseHandle(f->thread); f->thread = nullptr; }
 mir_cslock lck(m_csFiles);
 m_outgoingFiles.clear();
 m_incomingFiles.clear();
}

bool CKicqProto::FileProgress(OutgoingFile *file, uint64_t done)
{
 if (InterlockedCompareExchange(&file->cancelled, 0, 0)) return false;
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
    success = oft2::send_one(sock, prompt, input, f->size, [](uint64_t done, uint64_t, void *ctx) -> bool {
      auto *args = (OutgoingThreadArgs *)ctx;
      return args->proto->FileProgress(args->file.get(), done);
     }, ctx.get());
   }
   fclose(input);
  }
  closesocket(sock);
 }
 f->listener.close();
 proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
  success ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
 return 0;
}

HANDLE CKicqProto::SendFile(MCONTACT contact, const wchar_t *, wchar_t **files)
{
 if (!m_bOnline || !contact || !files || !files[0] || files[1]) return nullptr;
 const uint32_t uin = getDword(contact, "UIN");
 if (!uin) return nullptr;
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
 if (!f->listener.open()) return nullptr;
 uint64_t randomCookie = (uint64_t(GetTickCount()) << 16) ^ (uint64_t(rand()) << 32) ^ rand();
 for (int i = 0; i < 8; ++i) f->cookie[i] = uint8_t(randomCookie >> (56 - 8 * i));
 oft2::Rendezvous rv;
 rv.type = 0;
 rv.cookie = f->cookie;
 const bytes_t cap = OscarText::unhex(CAP_FILE);
 if (cap.size() != 16) return nullptr;
 memcpy(rv.capability.data(), cap.data(), 16);
 rv.port = f->listener.port();
 rv.requestNumber = 1;
 rv.externalIp = Oft2LocalIPv4();
 rv.internalIp = rv.externalIp;
 if (!rv.externalIp) return nullptr;
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
 bytes_t payload;
 if (!oft2::build_rendezvous(rv, payload)) return nullptr;
 auto ctx = new OutgoingThreadArgs{ this, f };
 f->thread = CreateThread(nullptr, 0, OutgoingFileThread, ctx, 0, nullptr);
 if (!f->thread) { delete ctx; return nullptr; }
 {
  mir_cslock lck(m_csFiles);
  m_outgoingFiles[f.get()] = f;
 }
 char recipient[20];
 _itoa(uin, recipient, 10);
 bytes_t cookie((char *)f->cookie.data(), 8);
 Send(OscarPackets::fileRendezvous(recipient, cookie, payload, m_nextMsgReq++));
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
 status.totalFiles = 1;
 status.totalBytes = f->size;
 status.totalProgress = done;
 status.currentFileSize = f->size;
 status.currentFileProgress = done;
 ProtoBroadcastAck(f->contact, ACKTYPE_FILE, ACKRESULT_DATA, f, (LPARAM)&status);
 return true;
}

DWORD WINAPI CKicqProto::IncomingFileThread(LPVOID param)
{
 std::unique_ptr<IncomingThreadArgs> arg((IncomingThreadArgs *)param);
 auto f = arg->file;
 bool ok = false;
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
 arg->proto->ProtoBroadcastAck(f->contact, ACKTYPE_FILE,
   ok ? ACKRESULT_SUCCESS : ACKRESULT_FAILED, f.get(), 0);
 return 0;
}

void CKicqProto::HandleFileOffer(MCONTACT contact, const oft2::Rendezvous &rv)
{
 if (!contact || !rv.hasOffer || rv.type != 0 || rv.offer.count != 1 ||
     !rv.port || (!rv.externalIp && !rv.internalIp)) return;
 auto f = std::make_shared<IncomingFile>();
 f->contact = contact;
 f->size = rv.offer.size;
 f->port = rv.port;
 f->ip = rv.externalIp ? rv.externalIp : rv.internalIp;
 f->cookie = rv.cookie;
 const int count = MultiByteToWideChar(CP_ACP, 0, rv.offer.name.c_str(), -1, nullptr, 0);
 if (count < 2 || count > 256) return;
 std::wstring name(count, L'\0');
 MultiByteToWideChar(CP_ACP, 0, rv.offer.name.c_str(), -1, &name[0], count);
 name.resize(count - 1);
 f->filename = name;
 {
  mir_cslock lock(m_csFiles);
  m_incomingFiles[f.get()] = f;
 }
 DB::FILE_BLOB blob(f->filename.c_str());
 blob.setSize(f->size);
 blob.setUserInfo(f.get());
 DB::EventInfo dbei;
 dbei.iTimestamp = time(nullptr);
 ProtoChainRecvFile(contact, blob, dbei);
}

HANDLE CKicqProto::FileAllow(MCONTACT, HANDLE transfer, const wchar_t *path)
{
 std::shared_ptr<IncomingFile> f;
 { mir_cslock lock(m_csFiles); auto it = m_incomingFiles.find(transfer);
   if (it == m_incomingFiles.end()) return nullptr;
   f = it->second; }
 if (!path || !*path || f->thread) return nullptr;
 f->destination = path;
 if (f->destination.back() != L'\\' && f->destination.back() != L'/') f->destination += L'\\';
 f->destination += f->filename;
 auto *args = new IncomingThreadArgs{ this, f };
 f->thread = CreateThread(nullptr, 0, IncomingFileThread, args, 0, nullptr);
 if (!f->thread) { delete args; return nullptr; }
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
  return 0;
 }
 auto incoming = m_incomingFiles.find(transfer);
 if (incoming == m_incomingFiles.end()) return 1;
 InterlockedExchange(&incoming->second->cancelled, 1);
 return 0;
}

int CKicqProto::FileResume(HANDLE, int, const wchar_t *)
{
 return 1;
}

MEVENT CKicqProto::RecvFile(MCONTACT hContact, DB::FILE_BLOB &blob, DB::EventInfo &dbei)
{
 // Serialize the incoming transfer metadata before storing the event.
 blob.write(dbei);
 return db_event_add(hContact, &dbei);
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

// Avatar Services (Miranda NG 0.96.x). Uses existing BART wire/session.
// Called by Miranda Avatar Service to obtain an actual cached contact image.
// An advertised hash alone is never reported as a successfully downloaded image.
INT_PTR MIR_CDECL CKicqProto::OnGetAvatarInfo(WPARAM, LPARAM lParam)
{
 auto *info = reinterpret_cast<PROTO_AVATAR_INFORMATION *>(lParam);
 if (!info || !info->hContact) return GAIR_NOAVATAR;
 const auto file = getMStringW(info->hContact, "BartAvatarPath");
 if (!file.IsEmpty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
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
 const auto cachedHash = getMStringA(info->hContact, "BartAvatarHash");
 const uint32_t avatarUin = getDword(info->hContact, "UIN");
 if (avatarUin && cachedHash.GetLength() == 32) {
  bytes_t hash;
  hash.reserve(16);
  bool valid = true;
  for (int i = 0; i < 32; i += 2) {
   auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
   };
   const int hi = nibble(cachedHash[i]);
   const int lo = nibble(cachedHash[i + 1]);
   if (hi < 0 || lo < 0) { valid = false; break; }
   hash.push_back(uint8_t((hi << 4) | lo));
  }
  if (valid) QueueBartAvatar(avatarUin, 1, hash);
 }
 // A queued request returns WAITFOR; on completion the BART worker broadcasts
 // ACKTYPE_AVATAR for this contact, not a global Extra Icons refresh.
 mir_cslock lock(m_csBart);
 for (const auto &pending : m_bartPending)
  if (FindContact((uint32_t)strtoul(pending.screenName.c_str(), nullptr, 10)) == info->hContact)
   return GAIR_WAITFOR;
 return GAIR_NOAVATAR;
}

INT_PTR MIR_CDECL CKicqProto::OnAvatarCaps(WPARAM wParam, LPARAM lParam)
{
 switch (wParam) {
 case AF_ENABLED: return m_bOnline ? 1 : 0;
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
