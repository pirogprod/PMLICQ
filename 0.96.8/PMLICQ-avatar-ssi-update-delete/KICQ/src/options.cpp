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

class CKicqAccountDlg : public CKicqDlgBase
{
	CCtrlEdit edtUin, edtPassword, edtServer, edtPort, edtGroup;

public:
	CKicqAccountDlg(CKicqProto *ppro, int iDlgId) :
		CKicqDlgBase(ppro, iDlgId),
		edtUin(this, IDC_UIN),
		edtPassword(this, IDC_PASSWORD),
		edtServer(this, IDC_SERVER),
		edtPort(this, IDC_PORT),
		edtGroup(this, IDC_GROUP)
	{
		CreateLink(edtUin, "UIN", DBVT_DWORD, 0);
		CreateLink(edtPassword, "Password", L"");
		CreateLink(edtServer, ppro->m_wszServer);
		CreateLink(edtPort, ppro->m_iPort);
		CreateLink(edtGroup, ppro->m_wszDefaultGroup);
	}

	bool OnInitDialog() override
	{
		// the XOR login of that ICQ era only uses the first 8 characters
		edtPassword.SendMsg(EM_LIMITTEXT, 8, 0);
		edtUin.SendMsg(EM_LIMITTEXT, 10, 0);
		return true;
	}
};

// Account-wide ICQ profile editor (the usual Miranda Options/Apply workflow).
class CKicqProfileDlg : public CKicqDlgBase
{
 CCtrlEdit edits[11];
public:
 CKicqProfileDlg(CKicqProto *proto) : CKicqDlgBase(proto, IDD_KICQ_PROFILE),
  edits { {this, IDC_KICQ_PROFILE_NICK}, {this, IDC_KICQ_PROFILE_FIRST},
   {this, IDC_KICQ_PROFILE_LAST}, {this, IDC_KICQ_PROFILE_EMAIL},
   {this, IDC_KICQ_PROFILE_CITY}, {this, IDC_KICQ_PROFILE_STATE},
   {this, IDC_KICQ_PROFILE_PHONE}, {this, IDC_KICQ_PROFILE_FAX},
   {this, IDC_KICQ_PROFILE_STREET}, {this, IDC_KICQ_PROFILE_CELL},
   {this, IDC_KICQ_PROFILE_ZIP} }
 {
  const char *fields[] = { "Nick", "FirstName", "LastName", "e-mail", "City", "State",
   "Phone", "Fax", "Street", "Cellular", "ZIP" };
  for (int i = 0; i < 11; ++i) CreateLink(edits[i], fields[i], L"");
 }
};

class CKicqExtendedProfileDlg : public CKicqDlgBase
{
 CCtrlEdit about, company, department, position, homepage, year, month, day;
public:
 CKicqExtendedProfileDlg(CKicqProto *proto) : CKicqDlgBase(proto, IDD_KICQ_PROFILE_EXT),
  about(this, IDC_KICQ_EXT_ABOUT), company(this, IDC_KICQ_EXT_COMPANY),
  department(this, IDC_KICQ_EXT_DEPT), position(this, IDC_KICQ_EXT_POSITION),
  homepage(this, IDC_KICQ_EXT_HOMEPAGE), year(this, IDC_KICQ_EXT_BIRTHYEAR),
  month(this, IDC_KICQ_EXT_BIRTHMONTH), day(this, IDC_KICQ_EXT_BIRTHDAY)
 {
  CreateLink(about, "About", L"");
  CreateLink(company, "Company", L"");
  CreateLink(department, "CompanyDepartment", L"");
  CreateLink(position, "CompanyPosition", L"");
  CreateLink(homepage, "Homepage", L"");
  CreateLink(year, "BirthYear", L"");
  CreateLink(month, "BirthMonth", L"");
  CreateLink(day, "BirthDay", L"");
 }
};

MWindow CKicqProto::OnCreateAccMgrUI(MWindow hwndParent)
{
	auto *pDlg = new CKicqAccountDlg(this, IDD_ACCMGRUI);
	pDlg->SetParent(hwndParent);
	pDlg->Show();
	return pDlg->GetHwnd();
}

int CKicqProto::OnOptionsInit(WPARAM wParam, LPARAM)
{
	OPTIONSDIALOGPAGE odp = {};
	odp.flags = ODPF_UNICODE;
	odp.szGroup.w = LPGENW("Network");
	odp.szTitle.w = m_tszUserName;
	odp.position = 1;
	odp.pDialog = new CKicqAccountDlg(this, IDD_OPTIONS);
	g_plugin.addOptions(wParam, &odp);

 OPTIONSDIALOGPAGE profile = {};
 profile.flags = ODPF_UNICODE;
 profile.szGroup.w = LPGENW("Network");
 profile.szTitle.w = m_tszUserName;
 profile.szTab.w = LPGENW("Personal information");
 profile.position = 2;
 profile.pDialog = new CKicqProfileDlg(this);
 g_plugin.addOptions(wParam, &profile);
 OPTIONSDIALOGPAGE extended = {};
 extended.flags = ODPF_UNICODE;
 extended.szGroup.w = LPGENW("Network");
 extended.szTitle.w = m_tszUserName;
 extended.szTab.w = LPGENW("Extended information");
 extended.position = 3;
 extended.pDialog = new CKicqExtendedProfileDlg(this);
 g_plugin.addOptions(wParam, &extended);
	return 0;
}


// StdUserInfo pages for editing the owner's profile.  These are real
// editor pages, not redirects into the protocol Options dialog.
// Server replies update the account DB; a short refresh timer keeps an
// unmodified editor in sync without destroying unsaved user input.
class CKicqUserInfoEditorDlg : public CUserInfoPageDlg
{
 CKicqProto *m_proto;
 bool m_extended, m_loading = false, m_dirty = false;
 static constexpr UINT_PTR TIMER_PROFILE = 0x4B11;

 struct Field { int control; const char *setting; };
 static const Field *Fields(bool extended, size_t &count)
 {
  static const Field basic[] = {
   { IDC_KICQ_PROFILE_NICK, "Nick" }, { IDC_KICQ_PROFILE_FIRST, "FirstName" },
   { IDC_KICQ_PROFILE_LAST, "LastName" }, { IDC_KICQ_PROFILE_EMAIL, "e-mail" },
   { IDC_KICQ_PROFILE_CITY, "City" }, { IDC_KICQ_PROFILE_STATE, "State" },
   { IDC_KICQ_PROFILE_PHONE, "Phone" }, { IDC_KICQ_PROFILE_FAX, "Fax" },
   { IDC_KICQ_PROFILE_STREET, "Street" }, { IDC_KICQ_PROFILE_CELL, "Cellular" },
   { IDC_KICQ_PROFILE_ZIP, "ZIP" }
  };
  static const Field extra[] = {
   { IDC_KICQ_EXT_ABOUT, "About" }, { IDC_KICQ_EXT_COMPANY, "Company" },
   { IDC_KICQ_EXT_DEPT, "CompanyDepartment" }, { IDC_KICQ_EXT_POSITION, "CompanyPosition" },
   { IDC_KICQ_EXT_HOMEPAGE, "Homepage" }, { IDC_KICQ_EXT_BIRTHYEAR, "BirthYear" },
   { IDC_KICQ_EXT_BIRTHMONTH, "BirthMonth" }, { IDC_KICQ_EXT_BIRTHDAY, "BirthDay" }
  };
  count = extended ? _countof(extra) : _countof(basic);
  return extended ? extra : basic;
 }

 void LoadFields()
 {
  if (m_dirty) return;
  m_loading = true;
  size_t count = 0;
  const Field *fields = Fields(m_extended, count);
  for (size_t i = 0; i < count; ++i) {
   CMStringW value = m_proto->getMStringW(fields[i].setting);
   wchar_t current[2048] = {};
   GetDlgItemTextW(m_hwnd, fields[i].control, current, _countof(current));
   if (value != current)
    SetDlgItemTextW(m_hwnd, fields[i].control, value.c_str());
  }
  m_loading = false;
 }

 void SaveFields()
 {
  if (!m_dirty) return;
  size_t count = 0;
  const Field *fields = Fields(m_extended, count);
  std::vector<std::pair<std::string, std::wstring>> changes;
  for (size_t i = 0; i < count; ++i) {
   HWND edit = GetDlgItem(m_hwnd, fields[i].control);
   int len = GetWindowTextLengthW(edit);
   std::wstring value(static_cast<size_t>(len) + 1, L'\0');
   GetWindowTextW(edit, &value[0], len + 1);
   changes.emplace_back(fields[i].setting, value.c_str());
  }
  m_proto->SaveOwnProfileFields(changes, m_extended);
  m_dirty = false;
 }

public:
 CKicqUserInfoEditorDlg(CKicqProto *proto, bool extended) :
  CUserInfoPageDlg(g_plugin, extended ? IDD_KICQ_PROFILE_EXT : IDD_KICQ_PROFILE),
  m_proto(proto), m_extended(extended) {}

 INT_PTR DlgProc(UINT msg, WPARAM wp, LPARAM lp) override
 {
  if (msg == WM_INITDIALOG) {
   INT_PTR result = CUserInfoPageDlg::DlgProc(msg, wp, lp);
   LoadFields(); // show cached values immediately
   // Request the authoritative owner profile when either page opens.
   m_proto->RefreshOwnProfile();
   SetTimer(m_hwnd, TIMER_PROFILE, 800, nullptr);
   return result;
  }
  if (msg == WM_TIMER && wp == TIMER_PROFILE) {
   LoadFields(); // server updates can arrive asynchronously
   return TRUE;
  }
  if (msg == WM_COMMAND && HIWORD(wp) == EN_CHANGE && !m_loading) {
   m_dirty = true;
   SendMessage(GetParent(m_hwnd), PSM_CHANGED, reinterpret_cast<WPARAM>(m_hwnd), 0);
  }
  if (msg == WM_NOTIFY && lp) {
   const auto *hdr = reinterpret_cast<const NMHDR *>(lp);
   if (hdr->code == PSN_APPLY) {
    SaveFields();
    SetWindowLongPtr(m_hwnd, DWLP_MSGRESULT, PSNRET_NOERROR);
    return TRUE;
   }
  }
  if (msg == WM_DESTROY) KillTimer(m_hwnd, TIMER_PROFILE);
  return CUserInfoPageDlg::DlgProc(msg, wp, lp);
 }
};

int CKicqProto::OnUserInfoInit(WPARAM wParam, LPARAM lParam)
{
 // Only the own-user window; never publish editable pages for contacts.
 if (lParam != 0) return 0;
 // Use the configured UIN, including before the first OSCAR connection.
 // The page registration service copies the titles during AddPage.
 CMStringW basicTitle, extendedTitle;
 const uint32_t accountUin = getDword("UIN");
 if (accountUin != 0) {
  basicTitle.Format(L"%u - 1. Personal information", accountUin);
  extendedTitle.Format(L"%u - 2. Extended information", accountUin);
 }
 else {
  basicTitle = L"PMLICQ - 1. Personal information";
  extendedTitle = L"PMLICQ - 2. Extended information";
 }

 USERINFOPAGE basic = {};
 basic.flags = ODPF_UNICODE;
 basic.szTitle.w = const_cast<wchar_t*>(basicTitle.c_str());
 basic.szProto = m_szModuleName;
 basic.position = 1; // First: personal information
 basic.pDialog = new CKicqUserInfoEditorDlg(this, false);
 CallService("UserInfo/AddPage", wParam, reinterpret_cast<LPARAM>(&basic));

 USERINFOPAGE extended = {};
 extended.flags = ODPF_UNICODE;
 extended.szTitle.w = const_cast<wchar_t*>(extendedTitle.c_str());
 extended.szProto = m_szModuleName;
 extended.position = 2; // Second: extended information
 extended.pDialog = new CKicqUserInfoEditorDlg(this, true);
 CallService("UserInfo/AddPage", wParam, reinterpret_cast<LPARAM>(&extended));
 return 0;
}
