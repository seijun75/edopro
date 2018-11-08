#include "duelclient.h"
#include "client_card.h"
#include "materials.h"
#include "image_manager.h"
#include "single_mode.h"
#include "../ocgcore/common.h"
#include "game.h"
#include "replay.h"
#include "replay_mode.h"
#include <algorithm>

namespace ygo {

unsigned DuelClient::connect_state = 0;
unsigned char DuelClient::response_buf[64];
unsigned char DuelClient::response_len = 0;
unsigned int DuelClient::watching = 0;
unsigned char DuelClient::selftype = 0;
bool DuelClient::is_host = false;
event_base* DuelClient::client_base = 0;
bufferevent* DuelClient::client_bev = 0;
char DuelClient::duel_client_read[0x2000];
char DuelClient::duel_client_write[0x2000];
bool DuelClient::is_closing = false;
u64 DuelClient::select_hint = 0;
wchar_t DuelClient::event_string[256];
mtrandom DuelClient::rnd;

std::vector<ReplayPacket> DuelClient::replay_stream;
Replay DuelClient::last_replay;
bool DuelClient::old_replay = true;

bool DuelClient::is_refreshing = false;
int DuelClient::match_kill = 0;
std::vector<HostPacket> DuelClient::hosts;
std::set<unsigned int> DuelClient::remotes;
event* DuelClient::resp_event = 0;

unsigned int DuelClient::temp_ip = 0;
unsigned short DuelClient::temp_port = 0;
unsigned short DuelClient::temp_ver = 0;
bool DuelClient::try_needed = false;

bool DuelClient::StartClient(unsigned int ip, unsigned short port, bool create_game) {
	if(connect_state)
		return false;
	sockaddr_in sin;
	client_base = event_base_new();
	if(!client_base)
		return false;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(ip);
	sin.sin_port = htons(port);
	client_bev = bufferevent_socket_new(client_base, -1, BEV_OPT_CLOSE_ON_FREE);
	bufferevent_setcb(client_bev, ClientRead, NULL, ClientEvent, (void*)create_game);
	temp_ip = ip;
	temp_port = port;
	if (bufferevent_socket_connect(client_bev, (sockaddr*)&sin, sizeof(sin)) < 0) {
		bufferevent_free(client_bev);
		event_base_free(client_base);
		client_bev = 0;
		client_base = 0;
		return false;
	}
	connect_state = 0x1;
	rnd.reset(time(0));
	if(!create_game) {
		timeval timeout = {5, 0};
		event* resp_event = event_new(client_base, 0, EV_TIMEOUT, ConnectTimeout, 0);
		event_add(resp_event, &timeout);
	}
	Thread::NewThread(ClientThread, 0);
	return true;
}
void DuelClient::ConnectTimeout(evutil_socket_t fd, short events, void* arg) {
	if(connect_state == 0x7)
		return;
	if(!is_closing) {
		temp_ver = 0;
		mainGame->btnCreateHost->setEnabled(true);
		mainGame->btnJoinHost->setEnabled(true);
		mainGame->btnJoinCancel->setEnabled(true);
		mainGame->gMutex.Lock();
		if(!mainGame->wLanWindow->isVisible())
			mainGame->ShowElement(mainGame->wLanWindow);
		mainGame->env->addMessageBox(L"", dataManager.GetSysString(1400));
		mainGame->gMutex.Unlock();
	}
	event_base_loopbreak(client_base);
}
void DuelClient::StopClient(bool is_exiting) {
	if(connect_state != 0x7)
		return;
	is_closing = is_exiting;
	if(!is_closing) {

	}
	event_base_loopbreak(client_base);
}
void DuelClient::ClientRead(bufferevent* bev, void* ctx) {
	evbuffer* input = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(input);
	unsigned short packet_len = 0;
	while(true) {
		if(len < 2)
			return;
		evbuffer_copyout(input, &packet_len, 2);
		if(len < (size_t)packet_len + 2)
			return;
		evbuffer_remove(input, duel_client_read, packet_len + 2);
		if(packet_len)
			HandleSTOCPacketLan(&duel_client_read[2], packet_len);
		len -= packet_len + 2;
	}
}
void DuelClient::ClientEvent(bufferevent *bev, short events, void *ctx) {
	if (events & BEV_EVENT_CONNECTED) {
		bool create_game = (size_t)ctx != 0;
		CTOS_PlayerInfo cspi;
		BufferIO::CopyWStr(mainGame->ebNickName->getText(), cspi.name, 20);
		SendPacketToServer(CTOS_PLAYER_INFO, cspi);
		if(create_game) {
			CTOS_CreateGame cscg;
			BufferIO::CopyWStr(mainGame->ebServerName->getText(), cscg.name, 20);
			BufferIO::CopyWStr(mainGame->ebServerPass->getText(), cscg.pass, 20);
			cscg.info.rule = mainGame->cbRule->getSelected();
			cscg.info.mode = mainGame->cbMatchMode->getSelected();
			cscg.info.start_hand = _wtoi(mainGame->ebStartHand->getText());
			cscg.info.start_lp = _wtoi(mainGame->ebStartLP->getText());
			cscg.info.draw_count = _wtoi(mainGame->ebDrawCount->getText());
			cscg.info.time_limit = _wtoi(mainGame->ebTimeLimit->getText());
			cscg.info.lflist = mainGame->cbLFlist->getItemData(mainGame->cbLFlist->getSelected());
			cscg.info.duel_rule = mainGame->GetMasterRule(mainGame->duel_param, mainGame->forbiddentypes);
			cscg.info.duel_flag = mainGame->duel_param;
			cscg.info.no_check_deck = mainGame->chkNoCheckDeck->isChecked();
			cscg.info.no_shuffle_deck = mainGame->chkNoShuffleDeck->isChecked();
			cscg.info.check = 2;
			cscg.info.forbiddentypes = mainGame->forbiddentypes;
			cscg.info.extra_rules = mainGame->extra_rules;
			SendPacketToServer(CTOS_CREATE_GAME, cscg);
		} else {
			CTOS_JoinGame csjg;
			if (temp_ver)
				csjg.version = temp_ver;
			else
				csjg.version = PRO_VERSION;
			csjg.gameid = 0;
			BufferIO::CopyWStr(mainGame->ebJoinPass->getText(), csjg.pass, 20);
			SendPacketToServer(CTOS_JOIN_GAME, csjg);
		}
		bufferevent_enable(bev, EV_READ);
		connect_state |= 0x2;
	} else if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		bufferevent_disable(bev, EV_READ);
		if(!is_closing) {
			if(connect_state == 0x1) {
				temp_ver = 0;
				mainGame->btnCreateHost->setEnabled(true);
				mainGame->btnJoinHost->setEnabled(true);
				mainGame->btnJoinCancel->setEnabled(true);
				mainGame->gMutex.Lock();
				if(!mainGame->wLanWindow->isVisible())
					mainGame->ShowElement(mainGame->wLanWindow);
				mainGame->env->addMessageBox(L"", dataManager.GetSysString(1400));
				mainGame->gMutex.Unlock();
			} else if(connect_state == 0x7) {
				if(!mainGame->dInfo.isStarted && !mainGame->is_building) {
					mainGame->btnCreateHost->setEnabled(true);
					mainGame->btnJoinHost->setEnabled(true);
					mainGame->btnJoinCancel->setEnabled(true);
					mainGame->gMutex.Lock();
					mainGame->HideElement(mainGame->wHostPrepare);
					mainGame->HideElement(mainGame->wHostPrepare2);
					mainGame->ShowElement(mainGame->wLanWindow);
					mainGame->wChat->setVisible(false);
					if(events & BEV_EVENT_EOF)
						mainGame->env->addMessageBox(L"", dataManager.GetSysString(1401));
					else mainGame->env->addMessageBox(L"", dataManager.GetSysString(1402));
					mainGame->gMutex.Unlock();
				} else {
					ReplayPrompt(false);
					mainGame->gMutex.Lock();
					mainGame->env->addMessageBox(L"", dataManager.GetSysString(1502));
					mainGame->btnCreateHost->setEnabled(true);
					mainGame->btnJoinHost->setEnabled(true);
					mainGame->btnJoinCancel->setEnabled(true);
					mainGame->stTip->setVisible(false);
					mainGame->gMutex.Unlock();
					mainGame->closeDoneSignal.Reset();
					mainGame->closeSignal.Set();
					mainGame->closeDoneSignal.Wait();
					mainGame->gMutex.Lock();
					mainGame->dInfo.isStarted = false;
					mainGame->is_building = false;
					mainGame->device->setEventReceiver(&mainGame->menuHandler);
					mainGame->ShowElement(mainGame->wLanWindow);
					mainGame->gMutex.Unlock();
				}
			}
		}
		event_base_loopexit(client_base, 0);
	}
}
int DuelClient::ClientThread(void* param) {
	event_base_dispatch(client_base);
	bufferevent_free(client_bev);
	event_base_free(client_base);
	client_bev = 0;
	client_base = 0;
	connect_state = 0;
	return 0;
}
void DuelClient::HandleSTOCPacketLan(char* data, unsigned int len) {
	char* pdata = data;
	unsigned char pktType = BufferIO::ReadUInt8(pdata);
	switch(pktType) {
	case STOC_GAME_MSG: {
		ClientAnalyze(pdata, len - 1);
		break;
	}
	case STOC_ERROR_MSG: {
		STOC_ErrorMsg* pkt = (STOC_ErrorMsg*)pdata;
		switch(pkt->msg) {
		case ERRMSG_JOINERROR: {
			temp_ver = 0;
			mainGame->btnCreateHost->setEnabled(true);
			mainGame->btnJoinHost->setEnabled(true);
			mainGame->btnJoinCancel->setEnabled(true);
			mainGame->gMutex.Lock();
			if(pkt->code == 0)
				mainGame->env->addMessageBox(L"", dataManager.GetSysString(1403));
			else if(pkt->code == 1)
				mainGame->env->addMessageBox(L"", dataManager.GetSysString(1404));
			else if(pkt->code == 2)
				mainGame->env->addMessageBox(L"", dataManager.GetSysString(1405));
			mainGame->gMutex.Unlock();
			event_base_loopbreak(client_base);
			break;
		}
		case ERRMSG_DECKERROR: {
			mainGame->gMutex.Lock();
			int mainmin = 40, mainmax = 60, extramax = 15, sidemax = 15;
			if (mainGame->cbDeckSelect2->isVisible()) {
				if (mainGame->dInfo.extraval & 0x1) {
					mainmin = 40;
					mainmax = 60;
					extramax = 10;
					sidemax = 0;
				} else {
					mainmin = 100;
					mainmax = 100;
					extramax = 30;
					sidemax = 30;
				}
			} else if (mainGame->dInfo.extraval & 0x1) {
				mainmin = 20;
				mainmax = 30;
				extramax = 10;
				sidemax = 0;
			}
			unsigned int code = pkt->code & 0xFFFFFFF;
			int flag = pkt->code >> 28;
			wchar_t msgbuf[256];
			switch(flag)
			{
			case DECKERROR_LFLIST: {
				myswprintf(msgbuf, dataManager.GetSysString(1407), dataManager.GetName(code));
				break;
			}
			case DECKERROR_OCGONLY: {
				myswprintf(msgbuf, dataManager.GetSysString(1413), dataManager.GetName(code));
				break;
			}
			case DECKERROR_TCGONLY: {
				myswprintf(msgbuf, dataManager.GetSysString(1414), dataManager.GetName(code));
				break;
			}
			case DECKERROR_UNKNOWNCARD: {
				myswprintf(msgbuf, dataManager.GetSysString(1415), dataManager.GetName(code), code);
				break;
			}
			case DECKERROR_CARDCOUNT: {
				myswprintf(msgbuf, dataManager.GetSysString(1416), dataManager.GetName(code));
				break;
			}
			case DECKERROR_MAINCOUNT: {
				myswprintf(msgbuf, dataManager.GetSysString(1417), mainmin, mainmax, code);
				break;
			}
			case DECKERROR_EXTRACOUNT: {
				if(code>0)
					myswprintf(msgbuf, dataManager.GetSysString(1418), extramax, code);
				else
					myswprintf(msgbuf, dataManager.GetSysString(1420));
				break;
			}
			case DECKERROR_SIDECOUNT: {
				myswprintf(msgbuf, dataManager.GetSysString(1419), sidemax, code);
				break;
			}
			case DECKERROR_FORBTYPE: {
				myswprintf(msgbuf, dataManager.GetSysString(1421));
				break;
			}
			default: {
				myswprintf(msgbuf, dataManager.GetSysString(1406));
				break;
			}
			}
			mainGame->env->addMessageBox(L"", msgbuf);
			mainGame->cbDeckSelect->setEnabled(true);
			mainGame->cbDeckSelect2->setEnabled(true);
			if(mainGame->dInfo.isTag || mainGame->dInfo.isRelay)
				mainGame->btnHostPrepDuelist->setEnabled(true);
			mainGame->gMutex.Unlock();
			break;
		}
		case ERRMSG_SIDEERROR: {
			mainGame->gMutex.Lock();
			mainGame->env->addMessageBox(L"", dataManager.GetSysString(1408));
			mainGame->gMutex.Unlock();
			break;
		}
		case ERRMSG_VERERROR: {
			if (temp_ver) {
				temp_ver = 0;
				mainGame->btnCreateHost->setEnabled(true);
				mainGame->btnJoinHost->setEnabled(true);
				mainGame->btnJoinCancel->setEnabled(true);
				mainGame->gMutex.Lock();
				wchar_t msgbuf[256];
				myswprintf(msgbuf, dataManager.GetSysString(1411), pkt->code >> 12, (pkt->code >> 4) & 0xff, pkt->code & 0xf);
				mainGame->env->addMessageBox(L"", msgbuf);
				mainGame->gMutex.Unlock();
				event_base_loopbreak(client_base);
			} else {
				event_base_loopbreak(client_base);
				temp_ver = pkt->code;
				try_needed = true;
			}
			break;
		}
		}
		break;
	}
	case STOC_SELECT_HAND: {
		mainGame->wHand->setVisible(true);
		break;
	}
	case STOC_SELECT_TP: {
		mainGame->gMutex.Lock();
		mainGame->ShowElement(mainGame->wFTSelect);
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_HAND_RESULT: {
		STOC_HandResult* pkt = (STOC_HandResult*)pdata;
		mainGame->stHintMsg->setVisible(false);
		mainGame->showcardcode = (pkt->res1 - 1) + ((pkt->res2 - 1) << 16);
		mainGame->showcarddif = 50;
		mainGame->showcardp = 0;
		mainGame->showcard = 100;
		mainGame->WaitFrameSignal(60);
		break;
	}
	case STOC_TP_RESULT: {
		break;
	}
	case STOC_CHANGE_SIDE: {
		mainGame->gMutex.Lock();
		mainGame->dInfo.isStarted = false;
		mainGame->dField.Clear();
		mainGame->is_building = true;
		mainGame->is_siding = true;
		mainGame->wChat->setVisible(false);
		mainGame->wPhase->setVisible(false);
		mainGame->wDeckEdit->setVisible(false);
		mainGame->wFilter->setVisible(false);
		mainGame->wSort->setVisible(false);
		mainGame->stTip->setVisible(false);
		mainGame->btnSideOK->setVisible(true);
		mainGame->btnSideShuffle->setVisible(true);
		mainGame->btnSideSort->setVisible(true);
		mainGame->btnSideReload->setVisible(true);
		if(mainGame->dInfo.player_type < 7)
			mainGame->btnLeaveGame->setVisible(false);
		mainGame->btnSpectatorSwap->setVisible(false);
		mainGame->btnChainIgnore->setVisible(false);
		mainGame->btnChainAlways->setVisible(false);
		mainGame->btnChainWhenAvail->setVisible(false);
		mainGame->btnCancelOrFinish->setVisible(false);
		mainGame->deckBuilder.result_string[0] = L'0';
		mainGame->deckBuilder.result_string[1] = 0;
		mainGame->deckBuilder.results.clear();
		mainGame->deckBuilder.hovered_code = 0;
		mainGame->deckBuilder.is_draging = false;
		deckManager.pre_deck = deckManager.current_deck;
		mainGame->device->setEventReceiver(&mainGame->deckBuilder);
		mainGame->dInfo.isFirst = mainGame->dInfo.player_type == 0;
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_WAITING_SIDE: {
		mainGame->gMutex.Lock();
		mainGame->dField.Clear();
		mainGame->stHintMsg->setText(dataManager.GetSysString(1409));
		mainGame->stHintMsg->setVisible(true);
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_JOIN_GAME: {
		temp_ver = 0;
		STOC_JoinGame* pkt = (STOC_JoinGame*)pdata;
		std::wstring str, str2;
		wchar_t msgbuf[256];
		myswprintf(msgbuf, L"%ls%ls\n", dataManager.GetSysString(1226), deckManager.GetLFListName(pkt->info.lflist));
		str.append(msgbuf);
		myswprintf(msgbuf, L"%ls%ls\n", dataManager.GetSysString(1225), dataManager.GetSysString(1240 + pkt->info.rule));
		str.append(msgbuf);
		myswprintf(msgbuf, L"%ls%ls\n", dataManager.GetSysString(1227), dataManager.GetSysString(1244 + pkt->info.mode));
		str.append(msgbuf);
		if(pkt->info.time_limit) {
			myswprintf(msgbuf, L"%ls%d\n", dataManager.GetSysString(1237), pkt->info.time_limit);
			str.append(msgbuf);
		}
		str.append(L"==========\n");
		myswprintf(msgbuf, L"%ls%d\n", dataManager.GetSysString(1231), pkt->info.start_lp);
		str.append(msgbuf);
		myswprintf(msgbuf, L"%ls%d\n", dataManager.GetSysString(1232), pkt->info.start_hand);
		str.append(msgbuf);
		myswprintf(msgbuf, L"%ls%d\n", dataManager.GetSysString(1233), pkt->info.draw_count);
		str.append(msgbuf);
		int rule;
		if (pkt->info.check == 2) {
			mainGame->dInfo.duel_field = mainGame->GetMasterRule(pkt->info.duel_flag, pkt->info.forbiddentypes, &rule);
		} else {
			rule = pkt->info.duel_rule;
			if (rule == 0) {
				mainGame->dInfo.duel_field = 3;
				rule = 3;
			} else
				mainGame->dInfo.duel_field = rule;
		}
		if (rule == 5) {
			uint32 filter = 0x100;
			for (int i = 0; i < 6; ++i, filter <<= 1)
				if (pkt->info.duel_flag & filter) {
					myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1631 + i));
					str2.append(msgbuf);
				}
			myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1630));
			str.append(msgbuf);
		} else if (rule != DEFAULT_DUEL_RULE) {
			myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1260 + rule - 1));
			str.append(msgbuf);
		}
		mainGame->dInfo.lua64 = pkt->info.check == 2;
		if(pkt->info.check == 2) {
			for(int flag = SEALED_DUEL, i = 0; flag < DECK_MASTER + 1; flag = flag << 1, i++)
				if(pkt->info.extra_rules & flag) {
					myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1132 + i));
					str2.append(msgbuf);
				}
		}
		if(pkt->info.no_check_deck) {
			myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1229));
			str.append(msgbuf);
		}
		if(pkt->info.no_shuffle_deck) {
			myswprintf(msgbuf, L"*%ls\n", dataManager.GetSysString(1230));
			str.append(msgbuf);
		}
		mainGame->gMutex.Lock();
		int x = (pkt->info.mode == 3) ? 60 : 0;
		mainGame->btnHostPrepOB->setRelativePosition(rect<s32>(10, 180 + x, 110, 205 + x));
		mainGame->stHostPrepOB->setRelativePosition(rect<s32>(10, 210 + x, 270, 230 + x));
		mainGame->stHostPrepRule->setRelativePosition(rect<s32>(280, 30, 460, 230 + x));
		mainGame->stDeckSelect->setRelativePosition(rect<s32>(10, 235 + x, 110, 255 + x));
		mainGame->cbDeckSelect->setRelativePosition(rect<s32>(120, 230 + x, 270, 255 + x));
		mainGame->cbDeckSelect2->setRelativePosition(rect<s32>(280, 230 + x, 430, 255 + x));
		mainGame->btnHostPrepReady->setRelativePosition(rect<s32>(170, 180 + x, 270, 205 + x));
		mainGame->btnHostPrepNotReady->setRelativePosition(rect<s32>(170, 180 + x, 270, 205 + x));
		mainGame->btnHostPrepStart->setRelativePosition(rect<s32>(230, 280 + x, 340, 305 + x));
		mainGame->btnHostPrepCancel->setRelativePosition(rect<s32>(350, 280 + x, 460, 305 + x));
		mainGame->wHostPrepare->setRelativePosition(mainGame->ResizeWin(270, 120, 750, 440 + x));
		mainGame->wHostPrepare2->setRelativePosition(mainGame->ResizeWin(750, 120, 950, 440 + x));
		switch (pkt->info.mode) {
		case 0:
		case 1: {
			mainGame->dInfo.isTag = false;
			mainGame->dInfo.isRelay = false;
			for (int i = 2; i < 6; i++) {
				mainGame->chkHostPrepReady[i]->setVisible(false);
				mainGame->stHostPrepDuelist[i]->setVisible(false);
			}
			break; 
		}
		case 2:{
			mainGame->dInfo.isTag = true;
			mainGame->dInfo.isRelay = false;
			for (int i = 2; i < 4; ++i) {
				mainGame->stHostPrepDuelist[i]->setRelativePosition(rect<s32>(40, 75 + i * 25, 240, 95 + i * 25));
				mainGame->btnHostPrepKick[i]->setRelativePosition(rect<s32>(10, 75 + i * 25, 30, 95 + i * 25));
				mainGame->chkHostPrepReady[i]->setRelativePosition(rect<s32>(250, 75 + i * 25, 270, 95 + i * 25));
			}
			for (int i = 2; i < 4; i++) {
				mainGame->chkHostPrepReady[i]->setVisible(true);
				mainGame->stHostPrepDuelist[i]->setVisible(true);
			}
			for (int i = 4; i < 6; i++) {
				mainGame->chkHostPrepReady[i]->setVisible(false);
				mainGame->stHostPrepDuelist[i]->setVisible(false);
			}
			break;
		}
		case 3: {
			mainGame->dInfo.isTag = false;
			mainGame->dInfo.isRelay = true;
			for (int i = 0; i < 3; ++i) {
				mainGame->stHostPrepDuelist[i]->setRelativePosition(rect<s32>(40, 65 + i * 25, 240, 85 + i * 25));
				mainGame->btnHostPrepKick[i]->setRelativePosition(rect<s32>(10, 65 + i * 25, 30, 85 + i * 25));
				mainGame->chkHostPrepReady[i]->setRelativePosition(rect<s32>(250, 65 + i * 25, 270, 85 + i * 25));
			}
			for (int i = 3; i < 6; ++i) {
				mainGame->stHostPrepDuelist[i]->setRelativePosition(rect<s32>(40, 75 + i * 25, 240, 95 + i * 25));
				mainGame->btnHostPrepKick[i]->setRelativePosition(rect<s32>(10, 75 + i * 25, 30, 95 + i * 25));
				mainGame->chkHostPrepReady[i]->setRelativePosition(rect<s32>(250, 75 + i * 25, 270, 95 + i * 25));
			}
			for (int i = 2; i < 6; i++) {
				mainGame->chkHostPrepReady[i]->setVisible(true);
				mainGame->stHostPrepDuelist[i]->setVisible(true);
			}
			break;
		}
		}
		for(int i = 0; i < 6; ++i)
			mainGame->chkHostPrepReady[i]->setChecked(false);
		mainGame->btnHostPrepReady->setVisible(true);
		mainGame->btnHostPrepNotReady->setVisible(false);
		mainGame->dInfo.time_limit = pkt->info.time_limit;
		mainGame->dInfo.time_left[0] = 0;
		mainGame->dInfo.time_left[1] = 0;
		mainGame->deckBuilder.filterList = 0;
		for(auto lit = deckManager._lfList.begin(); lit != deckManager._lfList.end(); ++lit)
			if(lit->hash == pkt->info.lflist)
				mainGame->deckBuilder.filterList = lit->content;
		if(mainGame->deckBuilder.filterList == 0)
			mainGame->deckBuilder.filterList = deckManager._lfList[0].content;
		mainGame->stHostPrepDuelist[0]->setText(L"");
		mainGame->stHostPrepDuelist[1]->setText(L"");
		mainGame->stHostPrepDuelist[2]->setText(L"");
		mainGame->stHostPrepDuelist[3]->setText(L"");
		mainGame->stHostPrepDuelist[4]->setText(L"");
		mainGame->stHostPrepDuelist[5]->setText(L"");
		mainGame->stHostPrepOB->setText(L"");
		mainGame->stHostPrepRule->setText((wchar_t*)str.c_str());
		mainGame->stHostPrepRule2->setText((wchar_t*)str2.c_str());
		mainGame->RefreshDeck(mainGame->cbDeckSelect);
		mainGame->RefreshDeck(mainGame->cbDeckSelect2);
		mainGame->cbDeckSelect->setEnabled(true);
		if (pkt->info.check == 2 && pkt->info.extra_rules & DOUBLE_DECK) {
			mainGame->cbDeckSelect2->setVisible(true);
			mainGame->cbDeckSelect2->setEnabled(true);
		} else {
			mainGame->cbDeckSelect2->setVisible(false);
			mainGame->cbDeckSelect2->setEnabled(false);
		}
		if(mainGame->wCreateHost->isVisible())
			mainGame->HideElement(mainGame->wCreateHost);
		else if (mainGame->wLanWindow->isVisible())
			mainGame->HideElement(mainGame->wLanWindow);
		mainGame->ShowElement(mainGame->wHostPrepare);
		if(str2.size())
			mainGame->ShowElement(mainGame->wHostPrepare2);
		mainGame->wChat->setVisible(true);
		mainGame->gMutex.Unlock();
		mainGame->dInfo.extraval = (pkt->info.check == 2 && pkt->info.extra_rules & DUEL_SPEED) ? 1 : 0;
		if(mainGame->dInfo.isRelay)
			mainGame->dInfo.isFirst = mainGame->dInfo.player_type < 3;
		else if (mainGame->dInfo.isTag)
			mainGame->dInfo.isFirst = mainGame->dInfo.player_type < 2;
		else
			mainGame->dInfo.isFirst = mainGame->dInfo.player_type == 0;
		watching = 0;
		connect_state |= 0x4;
		break;
	}
	case STOC_TYPE_CHANGE: {
		STOC_TypeChange* pkt = (STOC_TypeChange*)pdata;
		if(!mainGame->dInfo.isTag && !mainGame->dInfo.isRelay) {
			selftype = pkt->type & 0xf;
			is_host = ((pkt->type >> 4) & 0xf) != 0;
			mainGame->btnHostPrepKick[2]->setVisible(false);
			mainGame->btnHostPrepKick[3]->setVisible(false);
			mainGame->btnHostPrepKick[4]->setVisible(false);
			mainGame->btnHostPrepKick[5]->setVisible(false);
			if(is_host) {
				mainGame->btnHostPrepStart->setVisible(true);
				mainGame->btnHostPrepKick[0]->setVisible(true);
				mainGame->btnHostPrepKick[1]->setVisible(true);
			} else {
				mainGame->btnHostPrepStart->setVisible(false);
				mainGame->btnHostPrepKick[0]->setVisible(false);
				mainGame->btnHostPrepKick[1]->setVisible(false);
			}
			mainGame->chkHostPrepReady[0]->setEnabled(false);
			mainGame->chkHostPrepReady[1]->setEnabled(false);
			if(selftype < 2) {
				mainGame->chkHostPrepReady[selftype]->setEnabled(true);
				mainGame->chkHostPrepReady[selftype]->setChecked(false);
				mainGame->btnHostPrepDuelist->setEnabled(false);
				mainGame->btnHostPrepOB->setEnabled(true);
				mainGame->btnHostPrepReady->setVisible(true);
				mainGame->btnHostPrepNotReady->setVisible(false);
			} else {
				mainGame->btnHostPrepDuelist->setEnabled(true);
				mainGame->btnHostPrepOB->setEnabled(false);
				mainGame->btnHostPrepReady->setVisible(false);
				mainGame->btnHostPrepNotReady->setVisible(false);
			}
			if(mainGame->chkHostPrepReady[0]->isChecked() && mainGame->chkHostPrepReady[1]->isChecked()) {
				mainGame->btnHostPrepStart->setEnabled(true);
			} else {
				mainGame->btnHostPrepStart->setEnabled(false);
			}
		} else if (mainGame->dInfo.isTag) {
			mainGame->btnHostPrepKick[4]->setVisible(false);
			mainGame->btnHostPrepKick[5]->setVisible(false);
			if(selftype < 4) {
				mainGame->chkHostPrepReady[selftype]->setEnabled(false);
				mainGame->chkHostPrepReady[selftype]->setChecked(false);
			}
			selftype = pkt->type & 0xf;
			is_host = ((pkt->type >> 4) & 0xf) != 0;
			mainGame->btnHostPrepDuelist->setEnabled(true);
			if(is_host) {
				mainGame->btnHostPrepStart->setVisible(true);
				for(int i = 0; i < 4; ++i)
					mainGame->btnHostPrepKick[i]->setVisible(true);
			} else {
				mainGame->btnHostPrepStart->setVisible(false);
				for(int i = 0; i < 4; ++i)
					mainGame->btnHostPrepKick[i]->setVisible(false);
			}
			if(selftype < 4) {
				mainGame->chkHostPrepReady[selftype]->setEnabled(true);
				mainGame->btnHostPrepOB->setEnabled(true);
				mainGame->btnHostPrepReady->setVisible(true);
				mainGame->btnHostPrepNotReady->setVisible(false);
			} else {
				mainGame->btnHostPrepOB->setEnabled(false);
				mainGame->btnHostPrepReady->setVisible(false);
				mainGame->btnHostPrepNotReady->setVisible(false);
			}
			if(mainGame->chkHostPrepReady[0]->isChecked() && mainGame->chkHostPrepReady[1]->isChecked()
				&& mainGame->chkHostPrepReady[2]->isChecked() && mainGame->chkHostPrepReady[3]->isChecked()) {
				mainGame->btnHostPrepStart->setEnabled(true);
			} else {
				mainGame->btnHostPrepStart->setEnabled(false);
			}
		} else {
			if (selftype < 6) {
				mainGame->chkHostPrepReady[selftype]->setEnabled(false);
				mainGame->chkHostPrepReady[selftype]->setChecked(false);
			}
			selftype = pkt->type & 0xf;
			is_host = ((pkt->type >> 4) & 0xf) != 0;
			mainGame->btnHostPrepDuelist->setEnabled(true);
			if (is_host) {
				mainGame->btnHostPrepStart->setVisible(true);
				for (int i = 0; i < 6; ++i)
					mainGame->btnHostPrepKick[i]->setVisible(true);
			}
			else {
				mainGame->btnHostPrepStart->setVisible(false);
				for (int i = 0; i < 6; ++i)
					mainGame->btnHostPrepKick[i]->setVisible(false);
			}
			if (selftype < 6) {
				mainGame->chkHostPrepReady[selftype]->setEnabled(true);
				mainGame->btnHostPrepOB->setEnabled(true);
				mainGame->btnHostPrepReady->setVisible(true);
				mainGame->btnHostPrepNotReady->setVisible(false);
			}
			else {
				mainGame->btnHostPrepOB->setEnabled(false);
				mainGame->btnHostPrepReady->setVisible(false);
				mainGame->btnHostPrepNotReady->setVisible(false);
			}
			if ((mainGame->chkHostPrepReady[0]->isChecked() || mainGame->chkHostPrepReady[1]->isChecked() || mainGame->chkHostPrepReady[2]->isChecked()) &&
				(mainGame->chkHostPrepReady[3]->isChecked() || mainGame->chkHostPrepReady[4]->isChecked() || mainGame->chkHostPrepReady[5]->isChecked())) {
				mainGame->btnHostPrepStart->setEnabled(true);
			}
			else {
				mainGame->btnHostPrepStart->setEnabled(false);
			}
		}
		mainGame->dInfo.player_type = selftype;
		break;
	}
	case STOC_DUEL_START: {
		mainGame->HideElement(mainGame->wHostPrepare);
		mainGame->HideElement(mainGame->wHostPrepare2);
		mainGame->WaitFrameSignal(11);
		mainGame->gMutex.Lock();
		mainGame->dField.Clear();
		mainGame->dInfo.isStarted = true;
		mainGame->dInfo.lp[0] = 0;
		mainGame->dInfo.lp[1] = 0;
		mainGame->dInfo.strLP[0][0] = 0;
		mainGame->dInfo.strLP[1][0] = 0;
		mainGame->dInfo.turn = 0;
		mainGame->dInfo.time_left[0] = 0;
		mainGame->dInfo.time_left[1] = 0;
		mainGame->dInfo.time_player = 2;
		mainGame->dInfo.current_player[0] = 0;
		mainGame->dInfo.current_player[1] = 0;
		mainGame->dInfo.isReplaySwapped = false;
		mainGame->is_building = false;
		mainGame->wCardImg->setVisible(true);
		mainGame->wInfos->setVisible(true);
		mainGame->wPhase->setVisible(true);
		mainGame->btnSideOK->setVisible(false);
		mainGame->btnDP->setVisible(false);
		mainGame->btnSP->setVisible(false);
		mainGame->btnM1->setVisible(false);
		mainGame->btnBP->setVisible(false);
		mainGame->btnM2->setVisible(false);
		mainGame->btnEP->setVisible(false);
		mainGame->btnShuffle->setVisible(false);
		mainGame->btnSideShuffle->setVisible(false);
		mainGame->btnSideSort->setVisible(false);
		mainGame->btnSideReload->setVisible(false);
		mainGame->wChat->setVisible(true);
		mainGame->device->setEventReceiver(&mainGame->dField);
		mainGame->SetPhaseButtons();
		if(!mainGame->dInfo.isTag && !mainGame->dInfo.isRelay) {
			if(selftype > 1) {
				mainGame->dInfo.player_type = 7;
				mainGame->btnLeaveGame->setText(dataManager.GetSysString(1350));
				mainGame->btnLeaveGame->setVisible(true);
				mainGame->btnSpectatorSwap->setVisible(true);
			}
			if(selftype != 1) {
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[0]->getText(), mainGame->dInfo.hostname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[1]->getText(), mainGame->dInfo.clientname[0], 20);
			} else {
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[1]->getText(), mainGame->dInfo.hostname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[0]->getText(), mainGame->dInfo.clientname[0], 20);
			}
		} else if(mainGame->dInfo.isTag) {
			if(selftype > 3) {
				mainGame->dInfo.player_type = 7;
				mainGame->btnLeaveGame->setText(dataManager.GetSysString(1350));
				mainGame->btnLeaveGame->setVisible(true);
				mainGame->btnSpectatorSwap->setVisible(true);
			}
			if(selftype > 1 && selftype < 4) {
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[2]->getText(), mainGame->dInfo.hostname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[3]->getText(), mainGame->dInfo.hostname[1], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[0]->getText(), mainGame->dInfo.clientname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[1]->getText(), mainGame->dInfo.clientname[1], 20);
			} else {
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[0]->getText(), mainGame->dInfo.hostname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[1]->getText(), mainGame->dInfo.hostname[1], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[2]->getText(), mainGame->dInfo.clientname[0], 20);
				BufferIO::CopyWStr(mainGame->stHostPrepDuelist[3]->getText(), mainGame->dInfo.clientname[1], 20);
			}
			mainGame->dInfo.current_player[0] = 0;
			mainGame->dInfo.current_player[1] = 0;
		} else {
			if(selftype > 5) {
				mainGame->dInfo.player_type = 7;
				mainGame->btnLeaveGame->setText(dataManager.GetSysString(1350));
				mainGame->btnLeaveGame->setVisible(true);
				mainGame->btnSpectatorSwap->setVisible(true);
			}
			if(selftype > 2 && selftype < 6) {
				for (int i = 2; i >= 0; i--) {
					if (mainGame->chkHostPrepReady[i + 3]->isChecked())
						mainGame->dInfo.current_player[0] = i;
					if (mainGame->chkHostPrepReady[i]->isChecked())
						mainGame->dInfo.current_player[1] = i;
					BufferIO::CopyWStr(mainGame->stHostPrepDuelist[i + 3]->getText(), mainGame->dInfo.hostname[i], 20);
					BufferIO::CopyWStr(mainGame->stHostPrepDuelist[i]->getText(), mainGame->dInfo.clientname[i], 20);
				}
			} else {
				for (int i = 2; i >= 0; i--) {
					if(mainGame->chkHostPrepReady[i]->isChecked())
						mainGame->dInfo.current_player[0] = i;
					if(mainGame->chkHostPrepReady[i + 3]->isChecked())
						mainGame->dInfo.current_player[1] = i;
					BufferIO::CopyWStr(mainGame->stHostPrepDuelist[i]->getText(), mainGame->dInfo.hostname[i], 20);
					BufferIO::CopyWStr(mainGame->stHostPrepDuelist[i + 3]->getText(), mainGame->dInfo.clientname[i], 20);
				}
			}
		}
		mainGame->gMutex.Unlock();
		match_kill = 0;
		replay_stream.clear();
		old_replay = true;
		break;
	}
	case STOC_DUEL_END: {
		ReplayPrompt(old_replay);
		mainGame->gMutex.Lock();
		if(mainGame->dInfo.player_type < 7)
			mainGame->btnLeaveGame->setVisible(false);
		mainGame->btnSpectatorSwap->setVisible(false);
		mainGame->btnChainIgnore->setVisible(false);
		mainGame->btnChainAlways->setVisible(false);
		mainGame->btnChainWhenAvail->setVisible(false);
		mainGame->stMessage->setText(dataManager.GetSysString(1500));
		mainGame->btnCancelOrFinish->setVisible(false);
		mainGame->PopupElement(mainGame->wMessage);
		mainGame->gMutex.Unlock();
		mainGame->actionSignal.Reset();
		mainGame->actionSignal.Wait();
		mainGame->closeDoneSignal.Reset();
		mainGame->closeSignal.Set();
		mainGame->closeDoneSignal.Wait();
		mainGame->gMutex.Lock();
		mainGame->dInfo.isStarted = false;
		mainGame->is_building = false;
		mainGame->wDeckEdit->setVisible(false);
		mainGame->btnCreateHost->setEnabled(true);
		mainGame->btnJoinHost->setEnabled(true);
		mainGame->btnJoinCancel->setEnabled(true);
		mainGame->stTip->setVisible(false);
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
		mainGame->ShowElement(mainGame->wLanWindow);
		mainGame->gMutex.Unlock();
		event_base_loopbreak(client_base);
		if(exit_on_return)
			mainGame->device->closeDevice();
		break;
	}
	case STOC_REPLAY: {
		if(!old_replay) break;
		char* prep = pdata;
		replay_stream.push_back(ReplayPacket(OLD_REPLAY_MODE, prep, len - 1));
		break;
	}
	case STOC_TIME_LIMIT: {
		STOC_TimeLimit* pkt = (STOC_TimeLimit*)pdata;
		int lplayer = mainGame->LocalPlayer(pkt->player);
		if(lplayer == 0)
			DuelClient::SendPacketToServer(CTOS_TIME_CONFIRM);
		mainGame->dInfo.time_player = lplayer;
		mainGame->dInfo.time_left[lplayer] = pkt->left_time;
		break;
	}
	case STOC_CHAT: {
		STOC_Chat* pkt = (STOC_Chat*)pdata;
		int player = pkt->player;
		if(player < 6) {
			if(mainGame->dInfo.isTag) {
				if(mainGame->dInfo.isStarted && !mainGame->dInfo.isFirst)
					player ^= 2;
				player = (player > 1) ? (player % 2) * 2 + 1 : (player % 2) * 2;
				if(player > 3)
					player = 10;
			} else if(mainGame->dInfo.isRelay) {
				if (mainGame->dInfo.isStarted && !mainGame->dInfo.isFirst)
					player += (player > 2) ? -3 : 3;
				player = (player > 2) ? (player % 3) * 2 + 1 : (player % 3) * 2;
				if (player > 5)
					player = 10;
			} else {
				if(mainGame->dInfo.isStarted)
					player = mainGame->LocalPlayer(player);
			}
			if(mainGame->chkIgnore1->isChecked() && ((mainGame->dInfo.isFirst) ? player : player + 1) % 2)
				break;
		} else {
			if(player == 8) { //system custom message.
				if(mainGame->chkIgnore1->isChecked())
					break;
			} else if(player < 11 || player > 19) {
				if(mainGame->chkIgnore2->isChecked())
					break;
				player = 10;
			}
		}
		wchar_t msg[256];
		BufferIO::CopyWStr(pkt->msg, msg, 256);
		mainGame->gMutex.Lock();
		mainGame->AddChatMsg(msg, player);
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_HS_PLAYER_ENTER: {
		mainGame->PlaySoundEffect("./sound/playerenter.wav");
		STOC_HS_PlayerEnter* pkt = (STOC_HS_PlayerEnter*)pdata;
		if(pkt->pos > 5)
			break;
		wchar_t name[20];
		BufferIO::CopyWStr(pkt->name, name, 20);
		if (mainGame->dInfo.isRelay) {
			if (pkt->pos < 3)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.hostname[pkt->pos], 20);
			else
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.clientname[pkt->pos - 3], 20);
		} else if(mainGame->dInfo.isTag) {
			if(pkt->pos == 0)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.hostname[0], 20);
			else if(pkt->pos == 1)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.hostname[1], 20);
			else if(pkt->pos == 2)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.clientname[0], 20);
			else if(pkt->pos == 3)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.clientname[1], 20);
		} else {
			if(pkt->pos == 0)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.hostname[0], 20);
			else if(pkt->pos == 1)
				BufferIO::CopyWStr(pkt->name, mainGame->dInfo.clientname[0], 20);
		}
		mainGame->gMutex.Lock();
		mainGame->stHostPrepDuelist[pkt->pos]->setText(name);
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_HS_PLAYER_CHANGE: {
		STOC_HS_PlayerChange* pkt = (STOC_HS_PlayerChange*)pdata;
		unsigned char pos = (pkt->status >> 4) & 0xf;
		unsigned char state = pkt->status & 0xf;
		if(pos > 5)
			break;
		mainGame->gMutex.Lock();
		if(state < 8) {
			mainGame->PlaySoundEffect("./sound/playerenter.wav");
			wchar_t* prename = (wchar_t*)mainGame->stHostPrepDuelist[pos]->getText();
			mainGame->stHostPrepDuelist[state]->setText(prename);
			mainGame->stHostPrepDuelist[pos]->setText(L"");
			mainGame->chkHostPrepReady[pos]->setChecked(false);
			if (mainGame->dInfo.isRelay) {
				if (pos < 3)
					BufferIO::CopyWStr(L"", mainGame->dInfo.hostname[pos], 20);
				else
					BufferIO::CopyWStr(L"", mainGame->dInfo.clientname[pos - 3], 20);
				if (state < 3)
					BufferIO::CopyWStr(prename, mainGame->dInfo.hostname[state], 20);
				else
					BufferIO::CopyWStr(prename, mainGame->dInfo.clientname[state - 3], 20);
			} else {
				if (pos < 2)
					BufferIO::CopyWStr(L"", mainGame->dInfo.hostname[pos], 20);
				else if (pos < 4)
					BufferIO::CopyWStr(L"", mainGame->dInfo.clientname[pos - 2], 20);
				if (state < 2)
					BufferIO::CopyWStr(prename, mainGame->dInfo.hostname[state], 20);
				else if (state < 4)
					BufferIO::CopyWStr(prename, mainGame->dInfo.clientname[state - 2], 20);
			}
		} else if(state == PLAYERCHANGE_READY) {
			mainGame->chkHostPrepReady[pos]->setChecked(true);
			if(pos == selftype) {
				mainGame->btnHostPrepReady->setVisible(false);
				mainGame->btnHostPrepNotReady->setVisible(true);
			}
		} else if(state == PLAYERCHANGE_NOTREADY) {
			mainGame->chkHostPrepReady[pos]->setChecked(false);
			if(pos == selftype) {
				mainGame->btnHostPrepReady->setVisible(true);
				mainGame->btnHostPrepNotReady->setVisible(false);
			}
		} else if(state == PLAYERCHANGE_LEAVE) {
			mainGame->stHostPrepDuelist[pos]->setText(L"");
			mainGame->chkHostPrepReady[pos]->setChecked(false);
		} else if(state == PLAYERCHANGE_OBSERVE) {
			watching++;
			wchar_t watchbuf[32];
			myswprintf(watchbuf, L"%ls%d", dataManager.GetSysString(1253), watching);
			mainGame->stHostPrepDuelist[pos]->setText(L"");
			mainGame->chkHostPrepReady[pos]->setChecked(false);
			mainGame->stHostPrepOB->setText(watchbuf);
		}
		if((mainGame->chkHostPrepReady[0]->isChecked() && mainGame->chkHostPrepReady[1]->isChecked()
			&& (!mainGame->dInfo.isTag || (mainGame->chkHostPrepReady[2]->isChecked() && mainGame->chkHostPrepReady[3]->isChecked()))) || 
			(mainGame->dInfo.isRelay && ((mainGame->chkHostPrepReady[0]->isChecked() || mainGame->chkHostPrepReady[1]->isChecked() || mainGame->chkHostPrepReady[2]->isChecked()) &&
			(mainGame->chkHostPrepReady[3]->isChecked() || mainGame->chkHostPrepReady[4]->isChecked() || mainGame->chkHostPrepReady[5]->isChecked())))){
			mainGame->btnHostPrepStart->setEnabled(true);
		} else {
			mainGame->btnHostPrepStart->setEnabled(false);
		}
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_HS_WATCH_CHANGE: {
		STOC_HS_WatchChange* pkt = (STOC_HS_WatchChange*)pdata;
		watching = pkt->watch_count;
		wchar_t watchbuf[32];
		myswprintf(watchbuf, L"%ls%d", dataManager.GetSysString(1253), watching);
		mainGame->gMutex.Lock();
		mainGame->stHostPrepOB->setText(watchbuf);
		mainGame->gMutex.Unlock();
		break;
	}
	case STOC_NEW_REPLAY: {
		old_replay = false;
		char* prep = pdata;
		last_replay;
		memcpy(&last_replay.pheader, prep, sizeof(ReplayHeader));
		prep += sizeof(ReplayHeader);
		memcpy(last_replay.comp_data, prep, len - sizeof(ReplayHeader) - 1);
		last_replay.comp_size = len - sizeof(ReplayHeader) - 1;
		break;
	}
	}
}
int DuelClient::ClientAnalyze(char * msg, unsigned int len) {
	char* pbuf = msg;
	wchar_t textBuffer[256];
	if(!mainGame->dInfo.isReplay || mainGame->dInfo.isOldReplay) {
		mainGame->dInfo.curMsg = BufferIO::ReadUInt8(pbuf);
		if(mainGame->dInfo.curMsg != MSG_WAITING) {
			ReplayPacket p;
			p.message = mainGame->dInfo.curMsg;
			p.length = len - 1;
			memcpy(p.data, pbuf, p.length);
			replay_stream.push_back(p);
		}
	}
	mainGame->wCmdMenu->setVisible(false);
	if(!mainGame->dInfo.isReplay && mainGame->dInfo.curMsg != MSG_WAITING && mainGame->dInfo.curMsg != MSG_CARD_SELECTED) {
		mainGame->waitFrame = -1;
		mainGame->stHintMsg->setVisible(false);
		if(mainGame->wCardSelect->isVisible()) {
			mainGame->gMutex.Lock();
			mainGame->HideElement(mainGame->wCardSelect);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(11);
		}
		if(mainGame->wOptions->isVisible()) {
			mainGame->gMutex.Lock();
			mainGame->HideElement(mainGame->wOptions);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(11);
		}
	}
	if(mainGame->dInfo.time_player == 1)
		mainGame->dInfo.time_player = 2;
	switch(mainGame->dInfo.curMsg) {
	case MSG_RETRY: {
		mainGame->gMutex.Lock();
		mainGame->stMessage->setText(L"Error occurs.");
		mainGame->PopupElement(mainGame->wMessage);
		mainGame->gMutex.Unlock();
		mainGame->actionSignal.Reset();
		mainGame->actionSignal.Wait();
		mainGame->closeDoneSignal.Reset();
		mainGame->closeSignal.Set();
		mainGame->closeDoneSignal.Wait();
		ReplayPrompt(true);
		mainGame->gMutex.Lock();
		mainGame->dInfo.isStarted = false;
		mainGame->btnCreateHost->setEnabled(true);
		mainGame->btnJoinHost->setEnabled(true);
		mainGame->btnJoinCancel->setEnabled(true);
		mainGame->stTip->setVisible(false);
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
		mainGame->ShowElement(mainGame->wLanWindow);
		mainGame->gMutex.Unlock();
		event_base_loopbreak(client_base);
		if(exit_on_return)
			mainGame->device->closeDevice();
		return false;
	}
	case MSG_HINT: {
		int type = BufferIO::ReadInt8(pbuf);
		/*int player = */BufferIO::ReadInt8(pbuf);
		u64 data = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		switch (type) {
		case HINT_EVENT: {
			myswprintf(event_string, L"%ls", dataManager.GetDesc(data));
			break;
		}
		case HINT_MESSAGE: {
			mainGame->gMutex.Lock();
			mainGame->stMessage->setText(dataManager.GetDesc(data));
			mainGame->PopupElement(mainGame->wMessage);
			mainGame->gMutex.Unlock();
			mainGame->actionSignal.Reset();
			mainGame->actionSignal.Wait();
			break;
		}
		case HINT_SELECTMSG: {
			select_hint = data;
			break;
		}
		case HINT_OPSELECTED: {
			myswprintf(textBuffer, dataManager.GetSysString(1510), dataManager.GetDesc(data));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(0);
			mainGame->gMutex.Lock();
			mainGame->stACMessage->setText(textBuffer);
			mainGame->PopupElement(mainGame->wACMessage, 20);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(40);
			break;
		}
		case HINT_EFFECT: {
			mainGame->showcardcode = data;
			mainGame->showcarddif = 0;
			mainGame->showcard = 1;
			mainGame->WaitFrameSignal(30);
			break;
		}
		case HINT_RACE: {
			myswprintf(textBuffer, dataManager.GetSysString(1511), dataManager.FormatRace(data));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(0);
			mainGame->gMutex.Lock();
			mainGame->stACMessage->setText(textBuffer);
			mainGame->PopupElement(mainGame->wACMessage, 20);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(40);
			break;
		}
		case HINT_ATTRIB: {
			myswprintf(textBuffer, dataManager.GetSysString(1511), dataManager.FormatAttribute(data));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(0);
			mainGame->gMutex.Lock();
			mainGame->stACMessage->setText(textBuffer);
			mainGame->PopupElement(mainGame->wACMessage, 20);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(40);
			break;
		}
		case HINT_CODE: {
			myswprintf(textBuffer, dataManager.GetSysString(1511), dataManager.GetName(data));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(data);
			mainGame->gMutex.Lock();
			mainGame->stACMessage->setText(textBuffer);
			mainGame->PopupElement(mainGame->wACMessage, 20);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(40);
			break;
		}
		case HINT_NUMBER: {
			myswprintf(textBuffer, dataManager.GetSysString(1512), data);
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(0);
			mainGame->gMutex.Lock();
			mainGame->stACMessage->setText(textBuffer);
			mainGame->PopupElement(mainGame->wACMessage, 20);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(40);
			break;
		}
		case HINT_CARD: {
			mainGame->showcardcode = data;
			mainGame->showcarddif = 0;
			mainGame->showcard = 1;
			mainGame->WaitFrameSignal(30);
			break;
		}
		}
		break;
	}
	case MSG_WIN: {
		int player = BufferIO::ReadInt8(pbuf);
		int type = BufferIO::ReadInt8(pbuf);
		mainGame->showcarddif = 110;
		mainGame->showcardp = 0;
		mainGame->dInfo.vic_string = 0;
		wchar_t vic_buf[256];
		if(player == 2)
			mainGame->showcardcode = 3;
		else if(mainGame->LocalPlayer(player) == 0) {
			mainGame->showcardcode = 1;
			if(match_kill)
				myswprintf(vic_buf, dataManager.GetVictoryString(0x20), dataManager.GetName(match_kill));
			else if(type < 0x10)
				myswprintf(vic_buf, L"[%ls] %ls", mainGame->dInfo.clientname[mainGame->dInfo.current_player[1]], dataManager.GetVictoryString(type));
			else
				myswprintf(vic_buf, L"%ls", dataManager.GetVictoryString(type));
			mainGame->dInfo.vic_string = vic_buf;
		} else {
			mainGame->showcardcode = 2;
			if(match_kill)
				myswprintf(vic_buf, dataManager.GetVictoryString(0x20), dataManager.GetName(match_kill));
			else if(type < 0x10)
				myswprintf(vic_buf, L"[%ls] %ls", mainGame->dInfo.hostname[mainGame->dInfo.current_player[0]], dataManager.GetVictoryString(type));
			else
				myswprintf(vic_buf, L"%ls", dataManager.GetVictoryString(type));
			mainGame->dInfo.vic_string = vic_buf;
		}
		mainGame->showcard = 101;
		mainGame->WaitFrameSignal(120);
		mainGame->dInfo.vic_string = 0;
		mainGame->showcard = 0;
		break;
	}
	case MSG_WAITING: {
		mainGame->waitFrame = 0;
		mainGame->gMutex.Lock();
		mainGame->stHintMsg->setText(dataManager.GetSysString(1390));
		mainGame->stHintMsg->setVisible(true);
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_START: {
		int playertype = BufferIO::ReadInt8(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->showcardcode = 11;
			mainGame->showcarddif = 30;
			mainGame->showcardp = 0;
			mainGame->showcard = 101;
			mainGame->WaitFrameSignal(40);
			mainGame->showcard = 0;
			mainGame->gMutex.Lock();
			mainGame->dInfo.isFirst = (playertype & 0xf) ? false : true;
		}
		if(playertype & 0xf0)
			mainGame->dInfo.player_type = 7;
		if(mainGame->dInfo.isTag) {
			if(mainGame->dInfo.isFirst)
				mainGame->dInfo.current_player[1] = 1;
			else
				mainGame->dInfo.current_player[0] = 1;
		}
		mainGame->dInfo.lp[mainGame->LocalPlayer(0)] = BufferIO::ReadInt32(pbuf);
		mainGame->dInfo.lp[mainGame->LocalPlayer(1)] = BufferIO::ReadInt32(pbuf);
		if(mainGame->dInfo.lp[mainGame->LocalPlayer(0)] > 0)
			mainGame->dInfo.startlp = mainGame->dInfo.lp[mainGame->LocalPlayer(0)];
		else
			mainGame->dInfo.startlp = 8000;
		myswprintf(mainGame->dInfo.strLP[0], L"%d", mainGame->dInfo.lp[0]);
		myswprintf(mainGame->dInfo.strLP[1], L"%d", mainGame->dInfo.lp[1]);
		int deckc = BufferIO::ReadInt16(pbuf);
		int extrac = BufferIO::ReadInt16(pbuf);
		mainGame->dField.Initial(mainGame->LocalPlayer(0), deckc, extrac);
		deckc = BufferIO::ReadInt16(pbuf);
		extrac = BufferIO::ReadInt16(pbuf);
		mainGame->dField.Initial(mainGame->LocalPlayer(1), deckc, extrac);
		mainGame->dInfo.turn = 0;
		mainGame->dInfo.is_shuffling = false;
		if(mainGame->dInfo.isReplaySwapped) {
			std::swap(mainGame->dInfo.hostname, mainGame->dInfo.clientname);
			mainGame->dInfo.isReplaySwapped = false;
			mainGame->dField.ReplaySwap();
		}
		if (!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_UPDATE_DATA: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int location = BufferIO::ReadInt8(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Lock();
		mainGame->dField.UpdateFieldCard(player, location, pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_UPDATE_CARD: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int loc = BufferIO::ReadInt8(pbuf);
		int seq = BufferIO::ReadInt8(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Lock();
		mainGame->dField.UpdateCard(player, loc, seq, pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Unlock();
		break;
	}
	case MSG_SELECT_BATTLECMD: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		int code, count, con, loc, seq/*, diratt*/;
		u64 desc;
		ClientCard* pcard;
		mainGame->dField.activatable_cards.clear();
		mainGame->dField.activatable_descs.clear();
		mainGame->dField.conti_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc =  BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);;
			pcard = mainGame->dField.GetCard(con, loc, seq);
			int flag = 0;
			if(code & 0x80000000) {
				flag = EDESC_OPERATION;
				code &= 0x7fffffff;
			}
			mainGame->dField.activatable_cards.push_back(pcard);
			mainGame->dField.activatable_descs.push_back(std::make_pair(desc, flag));
			if(flag == EDESC_OPERATION) {
				pcard->chain_code = code;
				mainGame->dField.conti_cards.push_back(pcard);
				mainGame->dField.conti_act = true;
			} else {
				pcard->cmdFlag |= COMMAND_ACTIVATE;
				if (pcard->location == LOCATION_GRAVE)
					mainGame->dField.grave_act = true;
				else if (pcard->location == LOCATION_REMOVED)
					mainGame->dField.remove_act = true;
			}
		}
		mainGame->dField.attackable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			/*code = */BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = BufferIO::ReadInt8(pbuf);
			/*diratt = */BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.attackable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_ATTACK;
		}
		mainGame->gMutex.Lock();
		if(BufferIO::ReadInt8(pbuf)) {
			mainGame->btnM2->setVisible(true);
			mainGame->btnM2->setEnabled(true);
			mainGame->btnM2->setPressed(false);
		}
		if(BufferIO::ReadInt8(pbuf)) {
			mainGame->btnEP->setVisible(true);
			mainGame->btnEP->setEnabled(true);
			mainGame->btnEP->setPressed(false);
		}
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_IDLECMD: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		int code, count, con, loc, seq;
		u64 desc;
		ClientCard* pcard;
		mainGame->dField.summonable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.summonable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_SUMMON;
		}
		mainGame->dField.spsummonable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.spsummonable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_SPSUMMON;
			if (pcard->location == LOCATION_DECK) {
				pcard->SetCode(code);
				mainGame->dField.deck_act = true;
			} else if (pcard->location == LOCATION_GRAVE)
				mainGame->dField.grave_act = true;
			else if (pcard->location == LOCATION_REMOVED)
				mainGame->dField.remove_act = true;
			else if (pcard->location == LOCATION_EXTRA)
				mainGame->dField.extra_act = true;
			else {
				int seq = mainGame->dInfo.duel_field == 4 ? (mainGame->dInfo.extraval & 0x1) ? 1 : 0 : 6;
				if (pcard->location == LOCATION_SZONE && pcard->sequence == seq && (pcard->type & TYPE_PENDULUM) && !pcard->equipTarget)
					mainGame->dField.pzone_act[pcard->controler] = true;
			}
		}
		mainGame->dField.reposable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.reposable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_REPOS;
		}
		mainGame->dField.msetable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.msetable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_MSET;
		}
		mainGame->dField.ssetable_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(con, loc, seq);
			mainGame->dField.ssetable_cards.push_back(pcard);
			pcard->cmdFlag |= COMMAND_SSET;
		}
		mainGame->dField.activatable_cards.clear();
		mainGame->dField.activatable_descs.clear();
		mainGame->dField.conti_cards.clear();
		count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			con = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			loc = BufferIO::ReadInt8(pbuf);
			seq = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);;
			pcard = mainGame->dField.GetCard(con, loc, seq);
			int flag = 0;
			if(code & 0x80000000) {
				flag = EDESC_OPERATION;
				code &= 0x7fffffff;
			}
			mainGame->dField.activatable_cards.push_back(pcard);
			mainGame->dField.activatable_descs.push_back(std::make_pair(desc, flag));
			if(flag == EDESC_OPERATION) {
				pcard->chain_code = code;
				mainGame->dField.conti_cards.push_back(pcard);
				mainGame->dField.conti_act = true;
			} else {
				pcard->cmdFlag |= COMMAND_ACTIVATE;
				if (pcard->location == LOCATION_GRAVE)
					mainGame->dField.grave_act = true;
				else if (pcard->location == LOCATION_REMOVED)
					mainGame->dField.remove_act = true;
			}
		}
		if(BufferIO::ReadInt8(pbuf)) {
			mainGame->btnBP->setVisible(true);
			mainGame->btnBP->setEnabled(true);
			mainGame->btnBP->setPressed(false);
		}
		if(BufferIO::ReadInt8(pbuf)) {
			mainGame->btnEP->setVisible(true);
			mainGame->btnEP->setEnabled(true);
			mainGame->btnEP->setPressed(false);
		}
		if (BufferIO::ReadInt8(pbuf)) {
			mainGame->btnShuffle->setVisible(true);
		} else {
			mainGame->btnShuffle->setVisible(false);
		}
		return false;
	}
	case MSG_SELECT_EFFECTYN: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		loc_info info = ClientCard::read_location_info(pbuf);
		info.controler = mainGame->LocalPlayer(info.controler);
		ClientCard* pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
		if (pcard->code != code)
			pcard->SetCode(code);
		if(info.location != LOCATION_DECK) {
			pcard->is_highlighting = true;
			mainGame->dField.highlighting_card = pcard;
		}
		u64 desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);;
		if(desc == 0) {
			wchar_t ynbuf[256];
			myswprintf(ynbuf, dataManager.GetSysString(200), dataManager.FormatLocation(info.location, info.sequence), dataManager.GetName(code));
			myswprintf(textBuffer, L"%ls\n%ls", event_string, ynbuf);
		} else if(desc < 2048) {
			myswprintf(textBuffer, dataManager.GetSysString(desc), dataManager.GetName(code));
		} else {
			myswprintf(textBuffer, dataManager.GetDesc(desc), dataManager.GetName(code));
		}
		mainGame->gMutex.Lock();
		mainGame->stQMessage->setText(textBuffer);
		mainGame->PopupElement(mainGame->wQuery);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_YESNO: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		u64 desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);;
		mainGame->dField.highlighting_card = 0;
		mainGame->gMutex.Lock();
		mainGame->stQMessage->setText((wchar_t*)dataManager.GetDesc(desc));
		mainGame->PopupElement(mainGame->wQuery);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_OPTION: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_options.clear();
		for(int i = 0; i < count; ++i)
			mainGame->dField.select_options.push_back((mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf));
		mainGame->dField.ShowSelectOption(select_hint);
		select_hint = 0;
		return false;
	}
	case MSG_SELECT_CARD: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_cancelable = BufferIO::ReadInt8(pbuf) != 0;
		mainGame->dField.select_min = BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_max = BufferIO::ReadInt8(pbuf);
		int count = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectable_cards.clear();
		mainGame->dField.selected_cards.clear();
		unsigned int code;
		bool panelmode = false;
		bool select_ready = mainGame->dField.select_min == 0;
		mainGame->dField.select_ready = select_ready;
		ClientCard* pcard;
		for (int i = 0; i < count; ++i) {
			code = (unsigned int)BufferIO::ReadInt32(pbuf);
			loc_info info = ClientCard::read_location_info(pbuf);
			info.controler = mainGame->LocalPlayer(info.controler);
			if ((info.location & LOCATION_OVERLAY) > 0)
				pcard = mainGame->dField.GetCard(info.controler, info.location & (~LOCATION_OVERLAY) & 0xff, info.sequence)->overlayed[info.position];
			else if (info.location == 0) {
				pcard = new ClientCard();
				mainGame->dField.limbo_temp.push_back(pcard);
				panelmode = true;
			} else
				pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->select_seq = i;
			mainGame->dField.selectable_cards.push_back(pcard);
			pcard->is_selectable = true;
			pcard->is_selected = false;
			if (info.location & 0xf1)
				panelmode = true;
		}
		std::sort(mainGame->dField.selectable_cards.begin(), mainGame->dField.selectable_cards.end(), ClientCard::client_card_sort);
		if(select_hint)
			myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetDesc(select_hint),
			           mainGame->dField.select_min, mainGame->dField.select_max);
		else myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetSysString(560), mainGame->dField.select_min, mainGame->dField.select_max);
		select_hint = 0;
		if (panelmode) {
			mainGame->gMutex.Lock();
			mainGame->wCardSelect->setText(textBuffer);
			mainGame->dField.ShowSelectCard(select_ready);
			mainGame->gMutex.Unlock();
		} else {
			mainGame->stHintMsg->setText(textBuffer);
			mainGame->stHintMsg->setVisible(true);
		}
		if (mainGame->dField.select_cancelable) {
			mainGame->dField.ShowCancelOrFinishButton(1);
		} else if (select_ready) {
			mainGame->dField.ShowCancelOrFinishButton(2);
		} else {
			mainGame->dField.ShowCancelOrFinishButton(0);
		}
		return false;
	}
	case MSG_SELECT_UNSELECT_CARD: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		bool finishable = BufferIO::ReadInt8(pbuf) != 0;;
		bool cancelable = BufferIO::ReadInt8(pbuf) != 0;
		mainGame->dField.select_cancelable = finishable || cancelable;
		mainGame->dField.select_min = BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_max = BufferIO::ReadInt8(pbuf);
		int count1 = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectable_cards.clear();
		mainGame->dField.selected_cards.clear();
		unsigned int code;
		bool panelmode = false;
		mainGame->dField.select_ready = false;
		ClientCard* pcard;
		for (int i = 0; i < count1; ++i) {
			code = (unsigned int)BufferIO::ReadInt32(pbuf);
			loc_info info = ClientCard::read_location_info(pbuf);
			info.controler = mainGame->LocalPlayer(info.controler);
			if ((info.location & LOCATION_OVERLAY) > 0)
				pcard = mainGame->dField.GetCard(info.controler, info.location & (~LOCATION_OVERLAY) & 0xff, info.sequence)->overlayed[info.position];
			else if (info.location == 0) {
				pcard = new ClientCard();
				mainGame->dField.limbo_temp.push_back(pcard);
				panelmode = true;
			} else
				pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->select_seq = i;
			mainGame->dField.selectable_cards.push_back(pcard);
			pcard->is_selectable = true;
			pcard->is_selected = false;
			if (info.location & 0xf1)
				panelmode = true;
		}
		int count2 = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
		for (int i = count1; i < count1 + count2; ++i) {
			code = (unsigned int)BufferIO::ReadInt32(pbuf);
			loc_info info = ClientCard::read_location_info(pbuf);
			info.controler = mainGame->LocalPlayer(info.controler);
			if ((info.location & LOCATION_OVERLAY) > 0)
				pcard = mainGame->dField.GetCard(info.controler, info.location & (~LOCATION_OVERLAY) & 0xff, info.sequence)->overlayed[info.position];
			else if (info.location == 0) {
				pcard = new ClientCard();
				mainGame->dField.limbo_temp.push_back(pcard);
				panelmode = true;
			} else
				pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->select_seq = i;
			mainGame->dField.selectable_cards.push_back(pcard);
			pcard->is_selectable = true;
			pcard->is_selected = true;
			if (info.location & 0xf1)
				panelmode = true;
		}
		std::sort(mainGame->dField.selectable_cards.begin(), mainGame->dField.selectable_cards.end(), ClientCard::client_card_sort);
		if(select_hint)
			myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetDesc(select_hint),
			           mainGame->dField.select_min, mainGame->dField.select_max);
		else myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetSysString(560), mainGame->dField.select_min, mainGame->dField.select_max);
		select_hint = 0;
		if (panelmode) {
			mainGame->gMutex.Lock();
			mainGame->wCardSelect->setText(textBuffer);
			mainGame->dField.ShowSelectCard(mainGame->dField.select_cancelable);
			mainGame->gMutex.Unlock();
		} else {
			mainGame->stHintMsg->setText(textBuffer);
			mainGame->stHintMsg->setVisible(true);
		}
		if (mainGame->dField.select_cancelable) {
			if (count2 == 0)
				mainGame->dField.ShowCancelOrFinishButton(1);
			else
				mainGame->dField.ShowCancelOrFinishButton(2);
		}
		else
			mainGame->dField.ShowCancelOrFinishButton(0);
		return false;
	}
	case MSG_SELECT_CHAIN: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		int specount = BufferIO::ReadInt8(pbuf);
		int forced = BufferIO::ReadInt8(pbuf);
		/*int hint0 = */BufferIO::ReadInt32(pbuf);
		/*int hint1 = */BufferIO::ReadInt32(pbuf);
		int code;
		u64 desc;
		ClientCard* pcard;
		bool panelmode = false;
		bool conti_exist = false;
		bool select_trigger = (specount == 0x7f);
		mainGame->dField.chain_forced = (forced != 0);
		mainGame->dField.activatable_cards.clear();
		mainGame->dField.activatable_descs.clear();
		mainGame->dField.conti_cards.clear();
		for (int i = 0; i < count; ++i) {
			int flag = BufferIO::ReadInt8(pbuf);
			code = BufferIO::ReadInt32(pbuf);
			loc_info info = ClientCard::read_location_info(pbuf);
			info.controler = mainGame->LocalPlayer(info.controler);
			desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
			pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence, info.position);
			mainGame->dField.activatable_cards.push_back(pcard);
			mainGame->dField.activatable_descs.push_back(std::make_pair(desc, flag));
			pcard->is_selected = false;
			if(flag == EDESC_OPERATION) {
				pcard->chain_code = code;
				mainGame->dField.conti_cards.push_back(pcard);
				mainGame->dField.conti_act = true;
				conti_exist = true;
			} else {
				pcard->is_selectable = true;
				if(flag == EDESC_RESET)
					pcard->cmdFlag |= COMMAND_RESET;
				else
					pcard->cmdFlag |= COMMAND_ACTIVATE;
				if(pcard->location == LOCATION_DECK) {
					pcard->SetCode(code);
					mainGame->dField.deck_act = true;
				} else if(info.location == LOCATION_GRAVE)
					mainGame->dField.grave_act = true;
				else if(info.location == LOCATION_REMOVED)
					mainGame->dField.remove_act = true;
				else if(info.location == LOCATION_EXTRA)
					mainGame->dField.extra_act = true;
				else if(info.location == LOCATION_OVERLAY)
					panelmode = true;
			}
		}
		if(!select_trigger && !forced && (mainGame->ignore_chain || ((count == 0 || specount == 0) && !mainGame->always_chain)) && (count == 0 || !mainGame->chain_when_avail)) {
			SetResponseI(-1);
			mainGame->dField.ClearChainSelect();
			if(mainGame->chkWaitChain->isChecked() && !mainGame->ignore_chain) {
				mainGame->WaitFrameSignal(rnd.real() * 20 + 20);
			}
			DuelClient::SendResponse();
			return true;
		}
		if(mainGame->chkAutoChain->isChecked() && forced && !(mainGame->always_chain || mainGame->chain_when_avail)) {
			SetResponseI(0);
			mainGame->dField.ClearChainSelect();
			DuelClient::SendResponse();
			return true;
		}
		mainGame->gMutex.Lock();
		if(!conti_exist)
			mainGame->stHintMsg->setText(dataManager.GetSysString(550));
		else
			mainGame->stHintMsg->setText(dataManager.GetSysString(556));
		mainGame->stHintMsg->setVisible(true);
		if(panelmode) {
			mainGame->dField.list_command = COMMAND_ACTIVATE;
			mainGame->dField.selectable_cards = mainGame->dField.activatable_cards;
			std::sort(mainGame->dField.selectable_cards.begin(), mainGame->dField.selectable_cards.end());
			auto eit = std::unique(mainGame->dField.selectable_cards.begin(), mainGame->dField.selectable_cards.end());
			mainGame->dField.selectable_cards.erase(eit, mainGame->dField.selectable_cards.end());
			mainGame->dField.ShowChainCard();
		} else {
			if(!forced) {
				if(count == 0)
					myswprintf(textBuffer, L"%ls\n%ls", dataManager.GetSysString(201), dataManager.GetSysString(202));
				else
					myswprintf(textBuffer, L"%ls\n%ls", event_string, dataManager.GetSysString(203));
				mainGame->stQMessage->setText((wchar_t*)textBuffer);
				mainGame->PopupElement(mainGame->wQuery);
			}
		}
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_PLACE:
	case MSG_SELECT_DISFIELD: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_min = BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectable_field = ~BufferIO::ReadInt32(pbuf);
		mainGame->dField.selected_field = 0;
		unsigned char respbuf[64];
		int pzone = 0;
		if (mainGame->dInfo.curMsg == MSG_SELECT_PLACE) {
			if (select_hint) {
				myswprintf(textBuffer, dataManager.GetSysString(569), dataManager.GetName(select_hint));
			} else
				myswprintf(textBuffer, dataManager.GetSysString(560));
		} else {
			if (select_hint) {
				myswprintf(textBuffer, dataManager.GetDesc(select_hint));
			} else
				myswprintf(textBuffer, dataManager.GetSysString(570));
		}
		select_hint = 0;
		mainGame->stHintMsg->setText(textBuffer);
		mainGame->stHintMsg->setVisible(true);
		if (mainGame->dInfo.curMsg == MSG_SELECT_PLACE && (
			(mainGame->chkMAutoPos->isChecked() && mainGame->dField.selectable_field & 0x7f007f) ||
			(mainGame->chkSTAutoPos->isChecked() && !(mainGame->dField.selectable_field & 0x7f007f)))) {
			unsigned int filter;
			if (mainGame->dField.selectable_field & 0x7f) {
				respbuf[0] = mainGame->LocalPlayer(0);
				respbuf[1] = LOCATION_MZONE;
				filter = mainGame->dField.selectable_field & 0x7f;
			} else if (mainGame->dField.selectable_field & 0x1f00) {
				respbuf[0] = mainGame->LocalPlayer(0);
				respbuf[1] = LOCATION_SZONE;
				filter = (mainGame->dField.selectable_field >> 8) & 0x1f;
			} else if (mainGame->dField.selectable_field & 0xc000) {
				respbuf[0] = mainGame->LocalPlayer(0);
				respbuf[1] = LOCATION_SZONE;
				filter = (mainGame->dField.selectable_field >> 14) & 0x3;
				pzone = 1;
			} else if (mainGame->dField.selectable_field & 0x7f0000) {
				respbuf[0] = mainGame->LocalPlayer(1);
				respbuf[1] = LOCATION_MZONE;
				filter = (mainGame->dField.selectable_field >> 16) & 0x7f;
			} else if (mainGame->dField.selectable_field & 0x1f000000) {
				respbuf[0] = mainGame->LocalPlayer(1);
				respbuf[1] = LOCATION_SZONE;
				filter = (mainGame->dField.selectable_field >> 24) & 0x1f;
			} else {
				respbuf[0] = mainGame->LocalPlayer(1);
				respbuf[1] = LOCATION_SZONE;
				filter = (mainGame->dField.selectable_field >> 30) & 0x3;
				pzone = 1;
			}
			if(!pzone) {
				if(mainGame->chkRandomPos->isChecked()) {
					do {
						respbuf[2] = rnd.real() * 7;
					} while(!(filter & (1 << respbuf[2])));
				} else {
					if (filter & 0x40) respbuf[2] = 6;
					else if (filter & 0x20) respbuf[2] = 5;
					else if (filter & 0x4) respbuf[2] = 2;
					else if (filter & 0x2) respbuf[2] = 1;
					else if (filter & 0x8) respbuf[2] = 3;
					else if (filter & 0x1) respbuf[2] = 0;
					else if (filter & 0x10) respbuf[2] = 4;
				}
			} else {
				if (filter & 0x1) respbuf[2] = 6;
				else if (filter & 0x2) respbuf[2] = 7;
			}
			mainGame->dField.selectable_field = 0;
			SetResponseB(respbuf, 3);
			DuelClient::SendResponse();
			return true;
		}
		return false;
	}
	case MSG_SELECT_POSITION: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		int positions = BufferIO::ReadInt8(pbuf);
		if (positions == POS_FACEUP_ATTACK || positions == POS_FACEDOWN_ATTACK || positions == POS_FACEUP_DEFENSE || positions == POS_FACEDOWN_DEFENSE) {
			SetResponseI(positions);
			return true;
		}
		int count = 0, filter = 0x1, startpos;
		while(filter != 0x10) {
			if(positions & filter) count++;
			filter <<= 1;
		}
		if(count == 4) startpos = 10;
		else if(count == 3) startpos = 82;
		else startpos = 155;
		if(positions & POS_FACEUP_ATTACK) {
			mainGame->imageLoading.insert(std::make_pair(mainGame->btnPSAU, code));
			mainGame->btnPSAU->setRelativePosition(rect<s32>(startpos, 45, startpos + 140, 185));
			mainGame->btnPSAU->setVisible(true);
			startpos += 145;
		} else mainGame->btnPSAU->setVisible(false);
		if(positions & POS_FACEDOWN_ATTACK) {
			mainGame->btnPSAD->setRelativePosition(rect<s32>(startpos, 45, startpos + 140, 185));
			mainGame->btnPSAD->setVisible(true);
			startpos += 145;
		} else mainGame->btnPSAD->setVisible(false);
		if(positions & POS_FACEUP_DEFENSE) {
			mainGame->imageLoading.insert(std::make_pair(mainGame->btnPSDU, code));
			mainGame->btnPSDU->setRelativePosition(rect<s32>(startpos, 45, startpos + 140, 185));
			mainGame->btnPSDU->setVisible(true);
			startpos += 145;
		} else mainGame->btnPSDU->setVisible(false);
		if(positions & POS_FACEDOWN_DEFENSE) {
			mainGame->btnPSDD->setRelativePosition(rect<s32>(startpos, 45, startpos + 140, 185));
			mainGame->btnPSDD->setVisible(true);
			startpos += 145;
		} else mainGame->btnPSDD->setVisible(false);
		mainGame->gMutex.Lock();
		mainGame->PopupElement(mainGame->wPosSelect);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_TRIBUTE: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_cancelable = BufferIO::ReadInt8(pbuf) != 0;
		mainGame->dField.select_min = BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_max = BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt32(pbuf);
		mainGame->dField.selectable_cards.clear();
		mainGame->dField.selected_cards.clear();
		int c, l, s, t;
		unsigned int code;
		ClientCard* pcard;
		mainGame->dField.select_ready = false;
		for (int i = 0; i < count; ++i) {
			code = (unsigned int)BufferIO::ReadInt32(pbuf);
			c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			l = BufferIO::ReadInt8(pbuf);
			s = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			t = BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(c, l, s);
			if (code && pcard->code != code)
				pcard->SetCode(code);
			mainGame->dField.selectable_cards.push_back(pcard);
			pcard->opParam = t;
			pcard->select_seq = i;
			pcard->is_selectable = true;
		}
		if(select_hint)
			myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetDesc(select_hint),
			           mainGame->dField.select_min, mainGame->dField.select_max);
		else myswprintf(textBuffer, L"%ls(%d-%d)", dataManager.GetSysString(531), mainGame->dField.select_min, mainGame->dField.select_max);
		select_hint = 0;
		mainGame->gMutex.Lock();
		mainGame->stHintMsg->setText(textBuffer);
		mainGame->stHintMsg->setVisible(true);
		if (mainGame->dField.select_cancelable) {
			mainGame->dField.ShowCancelOrFinishButton(1);
		}
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_COUNTER: {
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_counter_type = BufferIO::ReadInt16(pbuf);
		mainGame->dField.select_counter_count = BufferIO::ReadInt16(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectable_cards.clear();
		int c, l, s, t/*, code*/;
		ClientCard* pcard;
		for (int i = 0; i < count; ++i) {
			/*code = */BufferIO::ReadInt32(pbuf);
			c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			l = BufferIO::ReadInt8(pbuf);
			s = BufferIO::ReadInt8(pbuf);
			t = BufferIO::ReadInt16(pbuf);
			pcard = mainGame->dField.GetCard(c, l, s);
			mainGame->dField.selectable_cards.push_back(pcard);
			pcard->opParam = (t << 16) | t;
			pcard->is_selectable = true;
		}
		myswprintf(textBuffer, dataManager.GetSysString(204), mainGame->dField.select_counter_count, dataManager.GetCounterName(mainGame->dField.select_counter_type));
		mainGame->gMutex.Lock();
		mainGame->stHintMsg->setText(textBuffer);
		mainGame->stHintMsg->setVisible(true);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_SELECT_SUM: {
		mainGame->dField.select_mode = BufferIO::ReadInt8(pbuf);
		/*int selecting_player = */BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_sumval = BufferIO::ReadInt32(pbuf);
		mainGame->dField.select_min = BufferIO::ReadInt8(pbuf);
		mainGame->dField.select_max = BufferIO::ReadInt8(pbuf);
		mainGame->dField.must_select_count = BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectsum_all.clear();
		mainGame->dField.selected_cards.clear();
		mainGame->dField.must_select_cards.clear();
		mainGame->dField.selectsum_cards.clear();
		for (int i = 0; i < mainGame->dField.must_select_count; ++i) {
			unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
			int c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			int l = BufferIO::ReadInt8(pbuf);
			int s = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			ClientCard* pcard = mainGame->dField.GetCard(c, l, s);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->opParam = BufferIO::ReadInt32(pbuf);
			pcard->select_seq = 0;
			mainGame->dField.must_select_cards.push_back(pcard);
		}
		int count = BufferIO::ReadInt8(pbuf);
		for (int i = 0; i < count; ++i) {
			unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
			int c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			int l = BufferIO::ReadInt8(pbuf);
			int s = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			ClientCard* pcard = mainGame->dField.GetCard(c, l, s);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->opParam = BufferIO::ReadInt32(pbuf);
			pcard->select_seq = i;
			mainGame->dField.selectsum_all.push_back(pcard);
		}
		std::sort(mainGame->dField.selectsum_all.begin(), mainGame->dField.selectsum_all.end(), ClientCard::client_card_sort);
		if(select_hint)
			myswprintf(textBuffer, L"%ls(%d)", dataManager.GetDesc(select_hint), mainGame->dField.select_sumval);
		else myswprintf(textBuffer, L"%ls(%d)", dataManager.GetSysString(560), mainGame->dField.select_sumval);
		select_hint = 0;
		mainGame->wCardSelect->setText(textBuffer);
		mainGame->stHintMsg->setText(textBuffer);
		return mainGame->dField.ShowSelectSum();
	}
	case MSG_SORT_CARD:
	case MSG_SORT_CHAIN: {
		/*int player = */BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		mainGame->dField.selectable_cards.clear();
		mainGame->dField.selected_cards.clear();
		mainGame->dField.sort_list.clear();
		int c, l, s;
		unsigned int code;
		ClientCard* pcard;
		for (int i = 0; i < count; ++i) {
			code = (unsigned int)BufferIO::ReadInt32(pbuf);
			c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			l = BufferIO::ReadInt8(pbuf);
			s = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			pcard = mainGame->dField.GetCard(c, l, s);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			mainGame->dField.selectable_cards.push_back(pcard);
			mainGame->dField.sort_list.push_back(0);
		}
		if (mainGame->chkAutoChain->isChecked() && mainGame->dInfo.curMsg == MSG_SORT_CHAIN) {
			mainGame->dField.sort_list.clear();
			SetResponseI(-1);
			DuelClient::SendResponse();
			return true;
		}
		if(mainGame->dInfo.curMsg == MSG_SORT_CHAIN)
			mainGame->wCardSelect->setText(dataManager.GetSysString(206));
		else
			mainGame->wCardSelect->setText(dataManager.GetSysString(205));
		mainGame->dField.select_min = 0;
		mainGame->dField.select_max = count;
		mainGame->dField.ShowSelectCard();
		return false;
	}
	case MSG_CONFIRM_DECKTOP: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		int code;
		ClientCard* pcard;
		mainGame->dField.selectable_cards.clear();
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			pbuf += (mainGame->dInfo.lua64) ? 6 : 3;
			pcard = *(mainGame->dField.deck[player].rbegin() + i);
			if (code != 0)
				pcard->SetCode(code);
		}
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		myswprintf(textBuffer, dataManager.GetSysString(207), count);
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(0);
		for (int i = 0; i < count; ++i) {
			pcard = *(mainGame->dField.deck[player].rbegin() + i);
			mainGame->gMutex.Lock();
			myswprintf(textBuffer, L"*[%ls]", dataManager.GetName(pcard->code));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(pcard->code);
			mainGame->gMutex.Unlock();
			float shift = -0.15f;
			if (player == 1) shift = 0.15f;
			pcard->dPos = irr::core::vector3df(shift, 0, 0);
			if(!mainGame->dField.deck_reversed && !pcard->is_reversed)
				pcard->dRot = irr::core::vector3df(0, 3.14159f / 5.0f, 0);
			else pcard->dRot = irr::core::vector3df(0, 0, 0);
			pcard->is_moving = true;
			pcard->aniFrame = 5;
			mainGame->WaitFrameSignal(45);
			mainGame->dField.MoveCard(pcard, 5);
			mainGame->WaitFrameSignal(5);
		}
		return true;
	}
	case MSG_CONFIRM_EXTRATOP: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		int code;
		ClientCard* pcard;
		mainGame->dField.selectable_cards.clear();
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			pbuf += (mainGame->dInfo.lua64) ? 6 : 3;
			pcard = *(mainGame->dField.extra[player].rbegin() + i + mainGame->dField.extra_p_count[player]);
			if (code != 0)
				pcard->SetCode(code);
		}
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		myswprintf(textBuffer, dataManager.GetSysString(207), count);
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(0);
		for (int i = 0; i < count; ++i) {
			pcard = *(mainGame->dField.extra[player].rbegin() + i + mainGame->dField.extra_p_count[player]);
			mainGame->gMutex.Lock();
			myswprintf(textBuffer, L"*[%ls]", dataManager.GetName(pcard->code));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(pcard->code);
			mainGame->gMutex.Unlock();
			if (player == 0)
				pcard->dPos = irr::core::vector3df(0, -0.20f, 0);
			else
				pcard->dPos = irr::core::vector3df(0.15f, 0, 0);
			pcard->dRot = irr::core::vector3df(0, 3.14159f / 5.0f, 0);
			pcard->is_moving = true;
			pcard->aniFrame = 5;
			mainGame->WaitFrameSignal(45);
			mainGame->dField.MoveCard(pcard, 5);
			mainGame->WaitFrameSignal(5);
		}
		return true;
	}
	case MSG_CONFIRM_CARDS: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		int code, c, l, s;
		std::vector<ClientCard*> field_confirm;
		std::vector<ClientCard*> panel_confirm;
		ClientCard* pcard;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pbuf += count * (mainGame->dInfo.lua64) ? 10 : 7;
			return true;
		}
		myswprintf(textBuffer, dataManager.GetSysString(208), count);
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(0);
		for (int i = 0; i < count; ++i) {
			code = BufferIO::ReadInt32(pbuf);
			c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
			l = BufferIO::ReadInt8(pbuf);
			s = (mainGame->dInfo.lua64) ? BufferIO::ReadInt32(pbuf) : BufferIO::ReadInt8(pbuf);
			if (l == 0) {
				pcard = new ClientCard();
				mainGame->dField.limbo_temp.push_back(pcard);
			} else
				pcard = mainGame->dField.GetCard(c, l, s);
			if (code != 0)
				pcard->SetCode(code);
			mainGame->gMutex.Lock();
			myswprintf(textBuffer, L"*[%ls]", dataManager.GetName(code));
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(code);
			mainGame->gMutex.Unlock();
			if (l & (LOCATION_EXTRA | LOCATION_DECK) || l == 0) {
				if(count == 1 && l != 0) {
					float shift = -0.15f;
					if (c == 0 && l == LOCATION_EXTRA) shift = 0.15f;
					pcard->dPos = irr::core::vector3df(shift, 0, 0);
					if(((l == LOCATION_DECK) && mainGame->dField.deck_reversed) || pcard->is_reversed || (pcard->position & POS_FACEUP))
						pcard->dRot = irr::core::vector3df(0, 0, 0);
					else pcard->dRot = irr::core::vector3df(0, 3.14159f / 5.0f, 0);
					pcard->is_moving = true;
					pcard->aniFrame = 5;
					mainGame->WaitFrameSignal(45);
					mainGame->dField.MoveCard(pcard, 5);
					mainGame->WaitFrameSignal(5);
				} else {
					if(!mainGame->dInfo.isReplay)
						panel_confirm.push_back(pcard);
				}
			} else {
				if(!mainGame->dInfo.isReplay || (l & LOCATION_ONFIELD))
					field_confirm.push_back(pcard);
			}
		}
		if (field_confirm.size() > 0) {
			mainGame->WaitFrameSignal(5);
			for(size_t i = 0; i < field_confirm.size(); ++i) {
				pcard = field_confirm[i];
				c = pcard->controler;
				l = pcard->location;
				if (l == LOCATION_HAND) {
					mainGame->dField.MoveCard(pcard, 5);
					pcard->is_highlighting = true;
				} else if (l == LOCATION_MZONE) {
					if (pcard->position & POS_FACEUP)
						continue;
					pcard->dPos = irr::core::vector3df(0, 0, 0);
					if (pcard->position == POS_FACEDOWN_ATTACK)
						pcard->dRot = irr::core::vector3df(0, 3.14159f / 5.0f, 0);
					else
						pcard->dRot = irr::core::vector3df(3.14159f / 5.0f, 0, 0);
					pcard->is_moving = true;
					pcard->aniFrame = 5;
				} else if (l == LOCATION_SZONE) {
					if (pcard->position & POS_FACEUP)
						continue;
					pcard->dPos = irr::core::vector3df(0, 0, 0);
					pcard->dRot = irr::core::vector3df(0, 3.14159f / 5.0f, 0);
					pcard->is_moving = true;
					pcard->aniFrame = 5;
				}
			}
			if (mainGame->dInfo.isReplay)
				mainGame->WaitFrameSignal(30);
			else
				mainGame->WaitFrameSignal(90);
			for(size_t i = 0; i < field_confirm.size(); ++i) {
				pcard = field_confirm[i];
				mainGame->dField.MoveCard(pcard, 5);
				pcard->is_highlighting = false;
			}
			mainGame->WaitFrameSignal(5);
		}
		if (panel_confirm.size()) {
			std::sort(panel_confirm.begin(), panel_confirm.end(), ClientCard::client_card_sort);
			mainGame->gMutex.Lock();
			mainGame->dField.selectable_cards = panel_confirm;
			myswprintf(textBuffer, dataManager.GetSysString(208), panel_confirm.size());
			mainGame->wCardSelect->setText(textBuffer);
			mainGame->dField.ShowSelectCard(true);
			mainGame->gMutex.Unlock();
			mainGame->actionSignal.Reset();
			mainGame->actionSignal.Wait();
		}
		return true;
	}
	case MSG_SHUFFLE_DECK: {
		mainGame->PlaySoundEffect("./sound/shuffle.wav");
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		if(mainGame->dField.deck[player].size() < 2)
			return true;
		bool rev = mainGame->dField.deck_reversed;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->dField.deck_reversed = false;
			if(rev) {
				for (size_t i = 0; i < mainGame->dField.deck[player].size(); ++i)
					mainGame->dField.MoveCard(mainGame->dField.deck[player][i], 10);
				mainGame->WaitFrameSignal(10);
			}
		}
		for (size_t i = 0; i < mainGame->dField.deck[player].size(); ++i) {
			mainGame->dField.deck[player][i]->code = 0;
			mainGame->dField.deck[player][i]->is_reversed = false;
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for (int i = 0; i < 5; ++i) {
				for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ++cit) {
					(*cit)->dPos = irr::core::vector3df(rand() * 0.4f / RAND_MAX - 0.2f, 0, 0);
					(*cit)->dRot = irr::core::vector3df(0, 0, 0);
					(*cit)->is_moving = true;
					(*cit)->aniFrame = 3;
				}
				mainGame->WaitFrameSignal(3);
				for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ++cit)
					mainGame->dField.MoveCard(*cit, 3);
				mainGame->WaitFrameSignal(3);
			}
			mainGame->dField.deck_reversed = rev;
			if(rev) {
				for (size_t i = 0; i < mainGame->dField.deck[player].size(); ++i)
					mainGame->dField.MoveCard(mainGame->dField.deck[player][i], 10);
			}
		}
		return true;
	}
	case MSG_SHUFFLE_HAND: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		/*int count = */BufferIO::ReadInt8(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->WaitFrameSignal(5);
			if(player == 1 && !mainGame->dInfo.isReplay && !mainGame->dInfo.isSingleMode) {
				bool flip = false;
				for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit)
					if((*cit)->code) {
						(*cit)->dPos = irr::core::vector3df(0, 0, 0);
						(*cit)->dRot = irr::core::vector3df(1.322f / 5, PI / 5, 0);
						(*cit)->is_moving = true;
						(*cit)->is_hovered = false;
						(*cit)->aniFrame = 5;
						flip = true;
					}
				if(flip)
					mainGame->WaitFrameSignal(5);
			}
			for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit) {
				(*cit)->dPos = irr::core::vector3df((3.9f - (*cit)->curPos.X) / 5, 0, 0);
				(*cit)->dRot = irr::core::vector3df(0, 0, 0);
				(*cit)->is_moving = true;
				(*cit)->is_hovered = false;
				(*cit)->aniFrame = 5;
			}
			mainGame->WaitFrameSignal(11);
		}
		for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit)
			(*cit)->SetCode(BufferIO::ReadInt32(pbuf));
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit) {
				(*cit)->is_hovered = false;
				mainGame->dField.MoveCard(*cit, 5);
			}
			mainGame->WaitFrameSignal(5);
		}
		return true;
	}
	case MSG_SHUFFLE_EXTRA: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		if((mainGame->dField.extra[player].size() - mainGame->dField.extra_p_count[player]) < 2)
			return true;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			if(count > 1)
				mainGame->PlaySoundEffect("./sound/shuffle.wav");
			for (int i = 0; i < 5; ++i) {
				for (auto cit = mainGame->dField.extra[player].begin(); cit != mainGame->dField.extra[player].end(); ++cit) {
					if(!((*cit)->position & POS_FACEUP)) {
						(*cit)->dPos = irr::core::vector3df(rand() * 0.4f / RAND_MAX - 0.2f, 0, 0);
						(*cit)->dRot = irr::core::vector3df(0, 0, 0);
						(*cit)->is_moving = true;
						(*cit)->aniFrame = 3;
					}
				}
				mainGame->WaitFrameSignal(3);
				for (auto cit = mainGame->dField.extra[player].begin(); cit != mainGame->dField.extra[player].end(); ++cit)
					if(!((*cit)->position & POS_FACEUP))
						mainGame->dField.MoveCard(*cit, 3);
				mainGame->WaitFrameSignal(3);
			}
		}
		for (auto cit = mainGame->dField.extra[player].begin(); cit != mainGame->dField.extra[player].end(); ++cit)
			if(!((*cit)->position & POS_FACEUP))
				(*cit)->SetCode(BufferIO::ReadInt32(pbuf));
		return true;
	}
	case MSG_REFRESH_DECK: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		return true;
	}
	case MSG_SWAP_GRAVE_DECK: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			mainGame->dField.grave[player].swap(mainGame->dField.deck[player]);
			for (auto cit = mainGame->dField.grave[player].begin(); cit != mainGame->dField.grave[player].end(); ++cit)
				(*cit)->location = LOCATION_GRAVE;
			int m = 0;
			for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ) {
				if ((*cit)->type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)) {
					(*cit)->position = POS_FACEDOWN;
					mainGame->dField.AddCard(*cit, player, LOCATION_EXTRA, 0);
					cit = mainGame->dField.deck[player].erase(cit);
				} else {
					(*cit)->location = LOCATION_DECK;
					(*cit)->sequence = m++;
					++cit;
				}
			}
		} else {
			mainGame->gMutex.Lock();
			mainGame->dField.grave[player].swap(mainGame->dField.deck[player]);
			for (auto cit = mainGame->dField.grave[player].begin(); cit != mainGame->dField.grave[player].end(); ++cit) {
				(*cit)->location = LOCATION_GRAVE;
				mainGame->dField.MoveCard(*cit, 10);
			}
			int m = 0;
			for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ) {
				ClientCard* pcard = *cit;
				if (pcard->type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ | TYPE_LINK)) {
					pcard->position = POS_FACEDOWN;
					mainGame->dField.AddCard(pcard, player, LOCATION_EXTRA, 0);
					cit = mainGame->dField.deck[player].erase(cit);
				} else {
					pcard->location = LOCATION_DECK;
					pcard->sequence = m++;
					++cit;
				}
				mainGame->dField.MoveCard(pcard, 10);
			}
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_REVERSE_DECK: {
		mainGame->dField.deck_reversed = !mainGame->dField.deck_reversed;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for(size_t i = 0; i < mainGame->dField.deck[0].size(); ++i)
				mainGame->dField.MoveCard(mainGame->dField.deck[0][i], 10);
			for(size_t i = 0; i < mainGame->dField.deck[1].size(); ++i)
				mainGame->dField.MoveCard(mainGame->dField.deck[1][i], 10);
		}
		return true;
	}
	case MSG_DECK_TOP: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int seq = BufferIO::ReadInt8(pbuf);
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		ClientCard* pcard = mainGame->dField.GetCard(player, LOCATION_DECK, mainGame->dField.deck[player].size() - 1 - seq);
		pcard->SetCode(code & 0x7fffffff);
		bool rev = (code & 0x80000000) != 0;
		if(pcard->is_reversed != rev) {
			pcard->is_reversed = rev;
			mainGame->dField.MoveCard(pcard, 5);
		}
		return true;
	}
	case MSG_SHUFFLE_SET_CARD: {
		std::vector<ClientCard*>* lst = 0;
		int loc = BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		if(loc == LOCATION_MZONE)
			lst = mainGame->dField.mzone;
		else
			lst = mainGame->dField.szone;
		ClientCard* mc[7];
		ClientCard* swp;
		int ps;
		for (int i = 0; i < count; ++i) {
			loc_info previous = ClientCard::read_location_info(pbuf);
			previous.controler = mainGame->LocalPlayer(previous.controler);
			mc[i] = lst[previous.controler][previous.sequence];
			mc[i]->SetCode(0);
			if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
				mc[i]->dPos = irr::core::vector3df((3.95f - mc[i]->curPos.X) / 10, 0, 0.05f);
				mc[i]->dRot = irr::core::vector3df(0, 0, 0);
				mc[i]->is_moving = true;
				mc[i]->aniFrame = 10;
			}
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->WaitFrameSignal(20);
		for (int i = 0; i < count; ++i) {
			loc_info previous = ClientCard::read_location_info(pbuf);
			previous.controler = mainGame->LocalPlayer(previous.controler);
			ps = mc[i]->sequence;
			if (previous.location > 0) {
				swp = lst[previous.controler][previous.sequence];
				lst[previous.controler][ps] = swp;
				lst[previous.controler][previous.sequence] = mc[i];
				mc[i]->sequence = previous.sequence;
				swp->sequence = ps;
			}
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for (int i = 0; i < count; ++i) {
				mainGame->dField.MoveCard(mc[i], 10);
				for (auto pcard : mc[i]->overlayed)
					mainGame->dField.MoveCard(pcard, 10);
			}
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_NEW_TURN: {
		mainGame->PlaySoundEffect("./sound/nextturn.wav");
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		mainGame->dInfo.turn++;
		if(!mainGame->dInfo.isTag && !mainGame->dInfo.isReplay && mainGame->dInfo.player_type < 7) {
			mainGame->btnLeaveGame->setText(dataManager.GetSysString(1351));
			mainGame->btnLeaveGame->setVisible(true);
		}
		if(!mainGame->dInfo.isReplay && mainGame->dInfo.player_type < 7) {
			if(!mainGame->chkHideHintButton->isChecked()) {
				mainGame->btnChainIgnore->setVisible(true);
				mainGame->btnChainAlways->setVisible(true);
				mainGame->btnChainWhenAvail->setVisible(true);
				//mainGame->dField.UpdateChainButtons();
			} else {
				mainGame->btnChainIgnore->setVisible(false);
				mainGame->btnChainAlways->setVisible(false);
				mainGame->btnChainWhenAvail->setVisible(false);
				mainGame->btnCancelOrFinish->setVisible(false);
			}
		}
		if(mainGame->dInfo.isTag && mainGame->dInfo.turn != 1) {
			if(player == 0)
				mainGame->dInfo.current_player[0] = (mainGame->dInfo.current_player[0] + 1) % 2;
			else
				mainGame->dInfo.current_player[1] = (mainGame->dInfo.current_player[1] + 1) % 2;
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->showcardcode = 10;
			mainGame->showcarddif = 30;
			mainGame->showcardp = 0;
			mainGame->showcard = 101;
			mainGame->WaitFrameSignal(40);
			mainGame->showcard = 0;
		}
		return true;
	}
	case MSG_NEW_PHASE: {
		mainGame->PlaySoundEffect("./sound/phase.wav");
		unsigned short phase = BufferIO::ReadInt16(pbuf);
		mainGame->btnDP->setVisible(false);
		mainGame->btnSP->setVisible(false);
		mainGame->btnM1->setVisible(false);
		mainGame->btnBP->setVisible(false);
		mainGame->btnM2->setVisible(false);
		mainGame->btnEP->setVisible(false);
		mainGame->btnShuffle->setVisible(false);
		mainGame->showcarddif = 30;
		mainGame->showcardp = 0;
		switch (phase) {
		case PHASE_DRAW:
			mainGame->btnDP->setVisible(true);
			mainGame->showcardcode = 4;
			break;
		case PHASE_STANDBY:
			mainGame->btnSP->setVisible(true);
			mainGame->showcardcode = 5;
			break;
		case PHASE_MAIN1:
			mainGame->btnM1->setVisible(true);
			mainGame->showcardcode = 6;
			break;
		case PHASE_BATTLE_START:
			mainGame->btnBP->setVisible(true);
			mainGame->btnBP->setPressed(true);
			mainGame->btnBP->setEnabled(false);
			mainGame->showcardcode = 7;
			break;
		case PHASE_MAIN2:
			mainGame->btnM2->setVisible(true);
			mainGame->btnM2->setPressed(true);
			mainGame->btnM2->setEnabled(false);
			mainGame->showcardcode = 8;
			break;
		case PHASE_END:
			mainGame->btnEP->setVisible(true);
			mainGame->btnEP->setPressed(true);
			mainGame->btnEP->setEnabled(false);
			mainGame->showcardcode = 9;
			break;
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->showcard = 101;
			mainGame->WaitFrameSignal(40);
			mainGame->showcard = 0;
		}
		return true;
	}
	case MSG_MOVE: {
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		loc_info previous = ClientCard::read_location_info(pbuf);
		previous.controler = mainGame->LocalPlayer(previous.controler);
		loc_info current = ClientCard::read_location_info(pbuf);
		current.controler = mainGame->LocalPlayer(current.controler);
		int reason = BufferIO::ReadInt32(pbuf);
		if (reason & REASON_DESTROY && previous.location != current.location)
			mainGame->PlaySoundEffect("./sound/destroyed.wav");
		if (previous.location == 0) {
			ClientCard* pcard = new ClientCard();
			pcard->position = current.position;
			pcard->SetCode(code);
			if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
				mainGame->gMutex.Lock();
				mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
				mainGame->gMutex.Unlock();
				mainGame->dField.GetCardLocation(pcard, &pcard->curPos, &pcard->curRot, true);
				pcard->curAlpha = 5;
				mainGame->dField.FadeCard(pcard, 255, 20);
				mainGame->WaitFrameSignal(20);
			} else
				mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
		} else if (current.location == 0) {
			ClientCard* pcard = mainGame->dField.GetCard(previous.controler, previous.location, previous.sequence);
			if (code != 0 && pcard->code != code)
				pcard->SetCode(code);
			pcard->ClearTarget();
			for(auto eqit = pcard->equipped.begin(); eqit != pcard->equipped.end(); ++eqit)
				(*eqit)->equipTarget = 0;
			if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
				mainGame->dField.FadeCard(pcard, 5, 20);
				mainGame->WaitFrameSignal(20);
				mainGame->gMutex.Lock();
				mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
				mainGame->gMutex.Unlock();
				if(pcard == mainGame->dField.hovered_card)
					mainGame->dField.hovered_card = 0;
			} else
				mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
			delete pcard;
		} else {
			if (!(previous.location & LOCATION_OVERLAY) && !(current.location & LOCATION_OVERLAY)) {
				ClientCard* pcard = mainGame->dField.GetCard(previous.controler, previous.location, previous.sequence);
				if (pcard->code != code && (code != 0 || current.location == LOCATION_EXTRA))
					pcard->SetCode(code);
				pcard->cHint = 0;
				pcard->chValue = 0;
				if((previous.location & LOCATION_ONFIELD) && (current.location != previous.location))
					pcard->counters.clear();
				if(current.location != previous.location) {
					pcard->ClearTarget();
					if(pcard->equipTarget) {
						pcard->equipTarget->is_showequip = false;
						pcard->equipTarget->equipped.erase(pcard);
						pcard->equipTarget = 0;
					}
				}
				pcard->is_hovered = false;
				pcard->is_showequip = false;
				pcard->is_showtarget = false;
				pcard->is_showchaintarget = false;
				if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
					mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
					pcard->position = current.position;
					mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
				} else {
					mainGame->gMutex.Lock();
					mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
					pcard->position = current.position;
					mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
					mainGame->gMutex.Unlock();
					if (previous.location == current.location && previous.controler == current.controler && (current.location & (LOCATION_DECK | LOCATION_GRAVE | LOCATION_REMOVED | LOCATION_EXTRA))) {
						pcard->dPos = irr::core::vector3df(-0.3f, 0, 0);
						pcard->dRot = irr::core::vector3df(0, 0, 0);
						if (previous.controler == 1) pcard->dPos.X = 0.3f;
						pcard->is_moving = true;
						pcard->aniFrame = 5;
						mainGame->WaitFrameSignal(5);
						mainGame->dField.MoveCard(pcard, 5);
						mainGame->WaitFrameSignal(5);
					} else {
						if (current.location == LOCATION_MZONE && pcard->overlayed.size() > 0) {
							mainGame->gMutex.Lock();
							for (size_t i = 0; i < pcard->overlayed.size(); ++i)
								mainGame->dField.MoveCard(pcard->overlayed[i], 10);
							mainGame->gMutex.Unlock();
							mainGame->WaitFrameSignal(10);
						}
						if (current.location == LOCATION_HAND) {
							mainGame->gMutex.Lock();
							for (size_t i = 0; i < mainGame->dField.hand[current.controler].size(); ++i)
								mainGame->dField.MoveCard(mainGame->dField.hand[current.controler][i], 10);
							mainGame->gMutex.Unlock();
						} else {
							mainGame->gMutex.Lock();
							mainGame->dField.MoveCard(pcard, 10);
							if (previous.location == LOCATION_HAND)
								for (size_t i = 0; i < mainGame->dField.hand[previous.controler].size(); ++i)
									mainGame->dField.MoveCard(mainGame->dField.hand[previous.controler][i], 10);
							mainGame->gMutex.Unlock();
						}
						mainGame->WaitFrameSignal(5);
					}
				}
			} else if (!(previous.location & LOCATION_OVERLAY)) {
				ClientCard* pcard = mainGame->dField.GetCard(previous.controler, previous.location, previous.sequence);
				if (code != 0 && pcard->code != code)
					pcard->SetCode(code);
				pcard->counters.clear();
				pcard->ClearTarget();
				pcard->is_showtarget = false;
				pcard->is_showchaintarget = false;
				ClientCard* olcard = mainGame->dField.GetCard(current.controler, current.location & (~LOCATION_OVERLAY) & 0xff, current.sequence);
				if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
					mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
					olcard->overlayed.push_back(pcard);
					mainGame->dField.overlay_cards.insert(pcard);
					pcard->overlayTarget = olcard;
					pcard->location = LOCATION_OVERLAY;
					pcard->sequence = olcard->overlayed.size() - 1;
				} else {
					mainGame->gMutex.Lock();
					mainGame->dField.RemoveCard(previous.controler, previous.location, previous.sequence);
					olcard->overlayed.push_back(pcard);
					mainGame->dField.overlay_cards.insert(pcard);
					mainGame->gMutex.Unlock();
					pcard->overlayTarget = olcard;
					pcard->location = LOCATION_OVERLAY;
					pcard->sequence = olcard->overlayed.size() - 1;
					if (olcard->location & LOCATION_ONFIELD) {
						mainGame->gMutex.Lock();
						mainGame->dField.MoveCard(pcard, 10);
						if (previous.location == LOCATION_HAND)
							for (size_t i = 0; i < mainGame->dField.hand[previous.controler].size(); ++i)
								mainGame->dField.MoveCard(mainGame->dField.hand[previous.controler][i], 10);
						mainGame->gMutex.Unlock();
						mainGame->WaitFrameSignal(5);
					}
				}
			} else if (!(current.location & LOCATION_OVERLAY)) {
				ClientCard* olcard = mainGame->dField.GetCard(previous.controler, previous.location & (~LOCATION_OVERLAY) & 0xff, previous.sequence);
				ClientCard* pcard = olcard->overlayed[previous.position];
				if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
					olcard->overlayed.erase(olcard->overlayed.begin() + pcard->sequence);
					pcard->overlayTarget = 0;
					pcard->position = current.position;
					mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
					mainGame->dField.overlay_cards.erase(pcard);
					for (size_t i = 0; i < olcard->overlayed.size(); ++i)
						olcard->overlayed[i]->sequence = i;
				} else {
					mainGame->gMutex.Lock();
					olcard->overlayed.erase(olcard->overlayed.begin() + pcard->sequence);
					pcard->overlayTarget = 0;
					pcard->position = current.position;
					mainGame->dField.AddCard(pcard, current.controler, current.location, current.sequence);
					mainGame->dField.overlay_cards.erase(pcard);
					for (size_t i = 0; i < olcard->overlayed.size(); ++i) {
						olcard->overlayed[i]->sequence = i;
						mainGame->dField.MoveCard(olcard->overlayed[i], 2);
					}
					mainGame->gMutex.Unlock();
					mainGame->WaitFrameSignal(5);
					mainGame->gMutex.Lock();
					mainGame->dField.MoveCard(pcard, 10);
					mainGame->gMutex.Unlock();
					mainGame->WaitFrameSignal(5);
				}
			} else {
				ClientCard* olcard1 = mainGame->dField.GetCard(previous.controler, previous.location & (~LOCATION_OVERLAY) & 0xff, previous.sequence);
				ClientCard* pcard = olcard1->overlayed[previous.position];
				ClientCard* olcard2 = mainGame->dField.GetCard(current.controler, current.location & (~LOCATION_OVERLAY) & 0xff, current.sequence);
				if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
					olcard1->overlayed.erase(olcard1->overlayed.begin() + pcard->sequence);
					olcard2->overlayed.push_back(pcard);
					pcard->sequence = olcard2->overlayed.size() - 1;
					pcard->location = LOCATION_OVERLAY;
					pcard->overlayTarget = olcard2;
					for (size_t i = 0; i < olcard1->overlayed.size(); ++i) {
						olcard1->overlayed[i]->sequence = i;
					}
				} else {
					mainGame->gMutex.Lock();
					olcard1->overlayed.erase(olcard1->overlayed.begin() + pcard->sequence);
					olcard2->overlayed.push_back(pcard);
					pcard->sequence = olcard2->overlayed.size() - 1;
					pcard->location = LOCATION_OVERLAY;
					pcard->overlayTarget = olcard2;
					for (size_t i = 0; i < olcard1->overlayed.size(); ++i) {
						olcard1->overlayed[i]->sequence = i;
						mainGame->dField.MoveCard(olcard1->overlayed[i], 2);
					}
					mainGame->dField.MoveCard(pcard, 10);
					mainGame->gMutex.Unlock();
					mainGame->WaitFrameSignal(5);
				}
			}
		}
		return true;
	}
	case MSG_POS_CHANGE: {
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		int cc = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int cl = BufferIO::ReadInt8(pbuf);
		int cs = BufferIO::ReadInt8(pbuf);
		int pp = BufferIO::ReadInt8(pbuf);
		int cp = BufferIO::ReadInt8(pbuf);
		ClientCard* pcard = mainGame->dField.GetCard(cc, cl, cs);
		if((pp & POS_FACEUP) && (cp & POS_FACEDOWN)) {
			pcard->counters.clear();
			pcard->ClearTarget();
		}
		if (code != 0 && pcard->code != code)
			pcard->SetCode(code);
		pcard->position = cp;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			myswprintf(event_string, dataManager.GetSysString(1600));
			mainGame->dField.MoveCard(pcard, 10);
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_SET: {
		mainGame->PlaySoundEffect("./sound/set.wav");
		/*int code = */BufferIO::ReadInt32(pbuf);
		/*loc_info info = */ClientCard::read_location_info(pbuf);
		myswprintf(event_string, dataManager.GetSysString(1601));
		return true;
	}
	case MSG_SWAP: {
		/*int code1 = */BufferIO::ReadInt32(pbuf);
		loc_info info1 = ClientCard::read_location_info(pbuf);
		info1.controler = mainGame->LocalPlayer(info1.controler);
		/*int code2 = */BufferIO::ReadInt32(pbuf);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		info2.controler = mainGame->LocalPlayer(info2.controler);
		myswprintf(event_string, dataManager.GetSysString(1602));
		ClientCard* pc1 = mainGame->dField.GetCard(info1.controler, info1.location, info1.sequence);
		ClientCard* pc2 = mainGame->dField.GetCard(info2.controler, info2.location, info2.sequence);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->gMutex.Lock();
			mainGame->dField.RemoveCard(info1.controler, info1.location, info1.sequence);
			mainGame->dField.RemoveCard(info2.controler, info2.location, info2.sequence);
			mainGame->dField.AddCard(pc1, info2.controler, info2.location, info2.sequence);
			mainGame->dField.AddCard(pc2, info1.controler, info1.location, info1.sequence);
			mainGame->dField.MoveCard(pc1, 10);
			mainGame->dField.MoveCard(pc2, 10);
			for (size_t i = 0; i < pc1->overlayed.size(); ++i)
				mainGame->dField.MoveCard(pc1->overlayed[i], 10);
			for (size_t i = 0; i < pc2->overlayed.size(); ++i)
				mainGame->dField.MoveCard(pc2->overlayed[i], 10);
			mainGame->gMutex.Unlock();
			mainGame->WaitFrameSignal(11);
		} else {
			mainGame->dField.RemoveCard(info1.controler, info1.location, info1.sequence);
			mainGame->dField.RemoveCard(info2.controler, info2.location, info2.sequence);
			mainGame->dField.AddCard(pc1, info2.controler, info2.location, info2.sequence);
			mainGame->dField.AddCard(pc2, info1.controler, info1.location, info1.sequence);
		}
		return true;
	}
	case MSG_FIELD_DISABLED: {
		unsigned int disabled = BufferIO::ReadInt32(pbuf);
		if (!mainGame->dInfo.isFirst)
			disabled = (disabled >> 16) | (disabled << 16);
		mainGame->dField.disabled_field = disabled;
		return true;
	}
	case MSG_SUMMONING: {
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		/*loc_info info = */ClientCard::read_location_info(pbuf);
		if(!mainGame->PlayChant(code))
			mainGame->PlaySoundEffect("./sound/summon.wav");
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			myswprintf(event_string, dataManager.GetSysString(1603), dataManager.GetName(code));
			mainGame->showcardcode = code;
			mainGame->showcarddif = 0;
			mainGame->showcardp = 0;
			mainGame->showcard = 7;
			mainGame->WaitFrameSignal(30);
			mainGame->showcard = 0;
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_SUMMONED: {
		myswprintf(event_string, dataManager.GetSysString(1604));
		return true;
	}
	case MSG_SPSUMMONING: {
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		/*loc_info info = */ClientCard::read_location_info(pbuf);
		if(!mainGame->PlayChant(code))
			mainGame->PlaySoundEffect("./sound/specialsummon.wav");
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			myswprintf(event_string, dataManager.GetSysString(1605), dataManager.GetName(code));
			mainGame->showcardcode = code;
			mainGame->showcarddif = 1;
			mainGame->showcard = 5;
			mainGame->WaitFrameSignal(30);
			mainGame->showcard = 0;
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_SPSUMMONED: {
		myswprintf(event_string, dataManager.GetSysString(1606));
		return true;
	}
	case MSG_FLIPSUMMONING: {
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		loc_info info = ClientCard::read_location_info(pbuf);
		info.controler = mainGame->LocalPlayer(info.controler);
		if(!mainGame->PlayChant(code))
			mainGame->PlaySoundEffect("./sound/flip.wav");
		ClientCard* pcard = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
		pcard->SetCode(code);
		pcard->position = info.position;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			myswprintf(event_string, dataManager.GetSysString(1607), dataManager.GetName(code));
			mainGame->dField.MoveCard(pcard, 10);
			mainGame->WaitFrameSignal(11);
			mainGame->showcardcode = code;
			mainGame->showcarddif = 0;
			mainGame->showcardp = 0;
			mainGame->showcard = 7;
			mainGame->WaitFrameSignal(30);
			mainGame->showcard = 0;
			mainGame->WaitFrameSignal(11);
		}
		return true;
	}
	case MSG_FLIPSUMMONED: {
		myswprintf(event_string, dataManager.GetSysString(1608));
		return true;
	}
	case MSG_CHAINING: {
		mainGame->PlaySoundEffect("./sound/activate.wav");
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		loc_info info = ClientCard::read_location_info(pbuf);
		int cc = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int cl = BufferIO::ReadInt8(pbuf);
		int cs = BufferIO::ReadInt8(pbuf);
		u64 desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
		/*int ct = */BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		ClientCard* pcard = mainGame->dField.GetCard(mainGame->LocalPlayer(info.controler), info.location, info.sequence, info.position);
		if(pcard->code != code) {
			pcard->code = code;
			mainGame->dField.MoveCard(pcard, 10);
		}
		mainGame->showcardcode = code;
		mainGame->showcarddif = 0;
		mainGame->showcard = 1;
		pcard->is_highlighting = true;
		if(pcard->location & 0x30) {
			float shift = -0.15f;
			if(cc == 1) shift = 0.15f;
			pcard->dPos = irr::core::vector3df(shift, 0, 0);
			pcard->dRot = irr::core::vector3df(0, 0, 0);
			pcard->is_moving = true;
			pcard->aniFrame = 5;
			mainGame->WaitFrameSignal(30);
			mainGame->dField.MoveCard(pcard, 5);
		} else
			mainGame->WaitFrameSignal(30);
		pcard->is_highlighting = false;
		mainGame->dField.current_chain.chain_card = pcard;
		mainGame->dField.current_chain.code = code;
		mainGame->dField.current_chain.desc = desc;
		mainGame->dField.current_chain.controler = cc;
		mainGame->dField.current_chain.location = cl;
		mainGame->dField.current_chain.sequence = cs;
		mainGame->dField.GetChainLocation(cc, cl, cs, &mainGame->dField.current_chain.chain_pos);
		mainGame->dField.current_chain.solved = false;
		mainGame->dField.current_chain.target.clear();
		int chc = 0;
		for(auto chit = mainGame->dField.chains.begin(); chit != mainGame->dField.chains.end(); ++chit) {
			if (cl == LOCATION_GRAVE || cl == LOCATION_REMOVED) {
				if (chit->controler == cc && chit->location == cl)
					chc++;
			} else {
				if (chit->controler == cc && chit->location == cl && chit->sequence == cs)
					chc++;
			}
		}
		if(cl == LOCATION_HAND)
			mainGame->dField.current_chain.chain_pos.X += 0.35f;
		else
			mainGame->dField.current_chain.chain_pos.Y += chc * 0.25f;
		return true;
	}
	case MSG_CHAINED: {
		int ct = BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		myswprintf(event_string, dataManager.GetSysString(1609), dataManager.GetName(mainGame->dField.current_chain.code));
		mainGame->gMutex.Lock();
		mainGame->dField.chains.push_back(mainGame->dField.current_chain);
		mainGame->gMutex.Unlock();
		if (ct > 1)
			mainGame->WaitFrameSignal(20);
		mainGame->dField.last_chain = true;
		return true;
	}
	case MSG_CHAIN_SOLVING: {
		int ct = BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		if (mainGame->dField.chains.size() > 1) {
			if (mainGame->dField.last_chain)
				mainGame->WaitFrameSignal(11);
			for(int i = 0; i < 5; ++i) {
				mainGame->dField.chains[ct - 1].solved = false;
				mainGame->WaitFrameSignal(3);
				mainGame->dField.chains[ct - 1].solved = true;
				mainGame->WaitFrameSignal(3);
			}
		}
		mainGame->dField.last_chain = false;
		return true;
	}
	case MSG_CHAIN_SOLVED: {
		/*int ct = */BufferIO::ReadInt8(pbuf);
		return true;
	}
	case MSG_CHAIN_END: {
		for(auto chit = mainGame->dField.chains.begin(); chit != mainGame->dField.chains.end(); ++chit) {
			for(auto tgit = chit->target.begin(); tgit != chit->target.end(); ++tgit)
				(*tgit)->is_showchaintarget = false;
			chit->chain_card->is_showchaintarget = false;
		}
		mainGame->dField.chains.clear();
		return true;
	}
	case MSG_CHAIN_NEGATED:
	case MSG_CHAIN_DISABLED: {
		int ct = BufferIO::ReadInt8(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			mainGame->showcardcode = mainGame->dField.chains[ct - 1].code;
			mainGame->showcarddif = 0;
			mainGame->showcard = 3;
			mainGame->WaitFrameSignal(30);
			mainGame->showcard = 0;
		}
		return true;
	}
	case MSG_CARD_SELECTED: {
		return true;
	}
	case MSG_RANDOM_SELECTED: {
		/*int player = */BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pbuf += count * 10;
			return true;
		}
		ClientCard* pcards[10];
		for (int i = 0; i < count; ++i) {
			loc_info info = ClientCard::read_location_info(pbuf);
			info.controler = mainGame->LocalPlayer(info.controler);
			if ((info.location & LOCATION_OVERLAY) > 0)
				pcards[i] = mainGame->dField.GetCard(info.controler, info.location & (~LOCATION_OVERLAY) & 0xff, info.sequence)->overlayed[info.position];
			else
				pcards[i] = mainGame->dField.GetCard(info.controler, info.location, info.sequence);
			pcards[i]->is_highlighting = true;
		}
		mainGame->WaitFrameSignal(30);
		for(int i = 0; i < count; ++i)
			pcards[i]->is_highlighting = false;
		return true;
	}
	case MSG_BECOME_TARGET: {
		int count = BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pbuf += count * 10;
			return true;
		}
		for (int i = 0; i < count; ++i) {
			loc_info info = ClientCard::read_location_info(pbuf);
			ClientCard* pcard = mainGame->dField.GetCard(mainGame->LocalPlayer(info.controler), info.location, info.sequence);
			pcard->is_highlighting = true;
			mainGame->dField.current_chain.target.insert(pcard);
			if(pcard->location & LOCATION_ONFIELD) {
				for (int j = 0; j < 3; ++j) {
					mainGame->dField.FadeCard(pcard, 5, 5);
					mainGame->WaitFrameSignal(5);
					mainGame->dField.FadeCard(pcard, 255, 5);
					mainGame->WaitFrameSignal(5);
				}
			} else if(pcard->location & 0x30) {
				float shift = -0.15f;
				if(info.controler == 1) shift = 0.15f;
				pcard->dPos = irr::core::vector3df(shift, 0, 0);
				pcard->dRot = irr::core::vector3df(0, 0, 0);
				pcard->is_moving = true;
				pcard->aniFrame = 5;
				mainGame->WaitFrameSignal(30);
				mainGame->dField.MoveCard(pcard, 5);
			} else
				mainGame->WaitFrameSignal(30);
			myswprintf(textBuffer, dataManager.GetSysString(1610), dataManager.GetName(pcard->code), dataManager.FormatLocation(info.location, info.sequence), info.sequence + 1);
			mainGame->lstLog->addItem(textBuffer);
			mainGame->logParam.push_back(pcard->code);
			pcard->is_highlighting = false;
		}
		return true;
	}
	case MSG_DRAW: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		ClientCard* pcard;
		for (int i = 0; i < count; ++i) {
			unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
			pcard = mainGame->dField.GetCard(player, LOCATION_DECK, mainGame->dField.deck[player].size() - 1 - i);
			if(!mainGame->dField.deck_reversed || code)
				pcard->SetCode(code & 0x7fffffff);
		}
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			for (int i = 0; i < count; ++i) {
				pcard = mainGame->dField.GetCard(player, LOCATION_DECK, mainGame->dField.deck[player].size() - 1);
				mainGame->dField.deck[player].erase(mainGame->dField.deck[player].end() - 1);
				mainGame->dField.AddCard(pcard, player, LOCATION_HAND, 0);
			}
		} else {
			for (int i = 0; i < count; ++i) {
				mainGame->PlaySoundEffect("./sound/draw.wav");
				mainGame->gMutex.Lock();
				pcard = mainGame->dField.GetCard(player, LOCATION_DECK, mainGame->dField.deck[player].size() - 1);
				mainGame->dField.deck[player].erase(mainGame->dField.deck[player].end() - 1);
				mainGame->dField.AddCard(pcard, player, LOCATION_HAND, 0);
				for(size_t i = 0; i < mainGame->dField.hand[player].size(); ++i)
					mainGame->dField.MoveCard(mainGame->dField.hand[player][i], 10);
				mainGame->gMutex.Unlock();
				mainGame->WaitFrameSignal(5);
			}
		}
		if (player == 0)
			myswprintf(event_string, dataManager.GetSysString(1611), count);
		else myswprintf(event_string, dataManager.GetSysString(1612), count);
		return true;
	}
	case MSG_DAMAGE: {
		mainGame->PlaySoundEffect("./sound/damage.wav");
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int val = BufferIO::ReadInt32(pbuf);
		int final = mainGame->dInfo.lp[player] - val;
		if (final < 0)
			final = 0;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			mainGame->dInfo.lp[player] = final;
			myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
			return true;
		}
		mainGame->lpd = (mainGame->dInfo.lp[player] - final) / 10;
		if (player == 0)
			myswprintf(event_string, dataManager.GetSysString(1613), val);
		else
			myswprintf(event_string, dataManager.GetSysString(1614), val);
		mainGame->lpccolor = 0xffff0000;
		mainGame->lpplayer = player;
		myswprintf(textBuffer, L"-%d", val);
		mainGame->lpcstring = textBuffer;
		mainGame->WaitFrameSignal(30);
		mainGame->lpframe = 10;
		mainGame->WaitFrameSignal(11);
		mainGame->lpcstring = 0;
		mainGame->dInfo.lp[player] = final;
		mainGame->gMutex.Lock();
		myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_RECOVER: {
		mainGame->PlaySoundEffect("./sound/gainlp.wav");
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int val = BufferIO::ReadInt32(pbuf);
		int final = mainGame->dInfo.lp[player] + val;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			mainGame->dInfo.lp[player] = final;
			myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
			return true;
		}
		mainGame->lpd = (mainGame->dInfo.lp[player] - final) / 10;
		if (player == 0)
			myswprintf(event_string, dataManager.GetSysString(1615), val);
		else
			myswprintf(event_string, dataManager.GetSysString(1616), val);
		mainGame->lpccolor = 0xff00ff00;
		mainGame->lpplayer = player;
		myswprintf(textBuffer, L"+%d", val);
		mainGame->lpcstring = textBuffer;
		mainGame->WaitFrameSignal(30);
		mainGame->lpframe = 10;
		mainGame->WaitFrameSignal(11);
		mainGame->lpcstring = 0;
		mainGame->dInfo.lp[player] = final;
		mainGame->gMutex.Lock();
		myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_EQUIP: {
		mainGame->PlaySoundEffect("./sound/equip.wav");
		loc_info info1 = ClientCard::read_location_info(pbuf);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		ClientCard* pc1 = mainGame->dField.GetCard(mainGame->LocalPlayer(info1.controler), info1.location, info1.sequence);
		ClientCard* pc2 = mainGame->dField.GetCard(mainGame->LocalPlayer(info2.controler), info2.location, info2.sequence);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			if(pc1->equipTarget)
				pc1->equipTarget->equipped.erase(pc1);
			pc1->equipTarget = pc2;
			pc2->equipped.insert(pc1);
		} else {
			mainGame->gMutex.Lock();
			if(pc1->equipTarget) {
				pc1->is_showequip = false;
				pc1->equipTarget->is_showequip = false;
				pc1->equipTarget->equipped.erase(pc1);
			}
			pc1->equipTarget = pc2;
			pc2->equipped.insert(pc1);
			if (mainGame->dField.hovered_card == pc1)
				pc2->is_showequip = true;
			else if (mainGame->dField.hovered_card == pc2)
				pc1->is_showequip = true;
			mainGame->gMutex.Unlock();
		}
		return true;
	}
	case MSG_LPUPDATE: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int val = BufferIO::ReadInt32(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			mainGame->dInfo.lp[player] = val;
			myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
			return true;
		}
		mainGame->lpd = (mainGame->dInfo.lp[player] - val) / 10;
		mainGame->lpplayer = player;
		mainGame->lpframe = 10;
		mainGame->WaitFrameSignal(11);
		mainGame->dInfo.lp[player] = val;
		mainGame->gMutex.Lock();
		myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_UNEQUIP: {
		loc_info info = ClientCard::read_location_info(pbuf);
		ClientCard* pc = mainGame->dField.GetCard(mainGame->LocalPlayer(info.controler), info.location, info.sequence);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pc->equipTarget->equipped.erase(pc);
			pc->equipTarget = 0;
		} else {
			mainGame->gMutex.Lock();
			if (mainGame->dField.hovered_card == pc)
				pc->equipTarget->is_showequip = false;
			else if (mainGame->dField.hovered_card == pc->equipTarget)
				pc->is_showequip = false;
			pc->equipTarget->equipped.erase(pc);
			pc->equipTarget = 0;
			mainGame->gMutex.Unlock();
		}
		return true;
	}
	case MSG_CARD_TARGET: {
		loc_info info1 = ClientCard::read_location_info(pbuf);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		ClientCard* pc1 = mainGame->dField.GetCard(mainGame->LocalPlayer(info1.controler), info1.location, info1.sequence);
		ClientCard* pc2 = mainGame->dField.GetCard(mainGame->LocalPlayer(info2.controler), info2.location, info2.sequence);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pc1->cardTarget.insert(pc2);
			pc2->ownerTarget.insert(pc1);
		} else {
			mainGame->gMutex.Lock();
			pc1->cardTarget.insert(pc2);
			pc2->ownerTarget.insert(pc1);
			if (mainGame->dField.hovered_card == pc1)
				pc2->is_showtarget = true;
			else if (mainGame->dField.hovered_card == pc2)
				pc1->is_showtarget = true;
			mainGame->gMutex.Unlock();
		}
		break;
	}
	case MSG_CANCEL_TARGET: {
		loc_info info1 = ClientCard::read_location_info(pbuf);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		ClientCard* pc1 = mainGame->dField.GetCard(mainGame->LocalPlayer(info1.controler), info1.location, info1.sequence);
		ClientCard* pc2 = mainGame->dField.GetCard(mainGame->LocalPlayer(info2.controler), info2.location, info2.sequence);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			pc1->cardTarget.erase(pc2);
			pc2->ownerTarget.erase(pc1);
		} else {
			mainGame->gMutex.Lock();
			pc1->cardTarget.erase(pc2);
			pc2->ownerTarget.erase(pc1);
			if (mainGame->dField.hovered_card == pc1)
				pc2->is_showtarget = false;
			else if (mainGame->dField.hovered_card == pc2)
				pc1->is_showtarget = false;
			mainGame->gMutex.Unlock();
		}
		break;
	}
	case MSG_PAY_LPCOST: {
		mainGame->PlaySoundEffect("./sound/damage.wav");
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int cost = BufferIO::ReadInt32(pbuf);
		int final = mainGame->dInfo.lp[player] - cost;
		if (final < 0)
			final = 0;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping) {
			mainGame->dInfo.lp[player] = final;
			myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
			return true;
		}
		mainGame->lpd = (mainGame->dInfo.lp[player] - final) / 10;
		mainGame->lpccolor = 0xff0000ff;
		mainGame->lpplayer = player;
		myswprintf(textBuffer, L"-%d", cost);
		mainGame->lpcstring = textBuffer;
		mainGame->WaitFrameSignal(30);
		mainGame->lpframe = 10;
		mainGame->WaitFrameSignal(11);
		mainGame->lpcstring = 0;
		mainGame->dInfo.lp[player] = final;
		mainGame->gMutex.Lock();
		myswprintf(mainGame->dInfo.strLP[player], L"%d", mainGame->dInfo.lp[player]);
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_ADD_COUNTER: {
		mainGame->PlaySoundEffect("./sound/addcounter.wav");
		int type = BufferIO::ReadInt16(pbuf);
		int c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int l = BufferIO::ReadInt8(pbuf);
		int s = BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt16(pbuf);
		ClientCard* pc = mainGame->dField.GetCard(c, l, s);
		if (pc->counters.count(type))
			pc->counters[type] += count;
		else pc->counters[type] = count;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		myswprintf(textBuffer, dataManager.GetSysString(1617), dataManager.GetName(pc->code), count, dataManager.GetCounterName(type));
		pc->is_highlighting = true;
		mainGame->gMutex.Lock();
		mainGame->stACMessage->setText(textBuffer);
		mainGame->PopupElement(mainGame->wACMessage, 20);
		mainGame->gMutex.Unlock();
		mainGame->WaitFrameSignal(40);
		pc->is_highlighting = false;
		return true;
	}
	case MSG_REMOVE_COUNTER: {
		mainGame->PlaySoundEffect("./sound/removecounter.wav");
		int type = BufferIO::ReadInt16(pbuf);
		int c = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int l = BufferIO::ReadInt8(pbuf);
		int s = BufferIO::ReadInt8(pbuf);
		int count = BufferIO::ReadInt16(pbuf);
		ClientCard* pc = mainGame->dField.GetCard(c, l, s);
		pc->counters[type] -= count;
		if (pc->counters[type] <= 0)
			pc->counters.erase(type);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		myswprintf(textBuffer, dataManager.GetSysString(1618), dataManager.GetName(pc->code), count, dataManager.GetCounterName(type));
		pc->is_highlighting = true;
		mainGame->gMutex.Lock();
		mainGame->stACMessage->setText(textBuffer);
		mainGame->PopupElement(mainGame->wACMessage, 20);
		mainGame->gMutex.Unlock();
		mainGame->WaitFrameSignal(40);
		pc->is_highlighting = false;
		return true;
	}
	case MSG_ATTACK: {
		mainGame->PlaySoundEffect("./sound/attack.wav");
		loc_info info1 = ClientCard::read_location_info(pbuf);
		info1.controler = mainGame->LocalPlayer(info1.controler);
		mainGame->dField.attacker = mainGame->dField.GetCard(info1.controler, info1.location, info1.sequence);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		info2.controler = mainGame->LocalPlayer(info2.controler);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		float sy;
		if (info2.location) {
			mainGame->dField.attack_target = mainGame->dField.GetCard(info2.controler, info2.location, info2.sequence);
			myswprintf(event_string, dataManager.GetSysString(1619), dataManager.GetName(mainGame->dField.attacker->code),
			           dataManager.GetName(mainGame->dField.attack_target->code));
			float xa = mainGame->dField.attacker->curPos.X;
			float ya = mainGame->dField.attacker->curPos.Y;
			float xd = mainGame->dField.attack_target->curPos.X;
			float yd = mainGame->dField.attack_target->curPos.Y;
			sy = (float)sqrt((xa - xd) * (xa - xd) + (ya - yd) * (ya - yd)) / 2;
			mainGame->atk_t = vector3df((xa + xd) / 2, (ya + yd) / 2, 0);
			if (info1.controler == 0)
				mainGame->atk_r = vector3df(0, 0, -atan((xd - xa) / (yd - ya)));
			else
				mainGame->atk_r = vector3df(0, 0, 3.1415926 - atan((xd - xa) / (yd - ya)));
		} else {
			myswprintf(event_string, dataManager.GetSysString(1620), dataManager.GetName(mainGame->dField.attacker->code));
			float xa = mainGame->dField.attacker->curPos.X;
			float ya = mainGame->dField.attacker->curPos.Y;
			float xd = 3.95f;
			float yd = 3.5f;
			if (info1.controler == 0)
				yd = -3.5f;
			sy = (float)sqrt((xa - xd) * (xa - xd) + (ya - yd) * (ya - yd)) / 2;
			mainGame->atk_t = vector3df((xa + xd) / 2, (ya + yd) / 2, 0);
			if (info1.controler == 0)
				mainGame->atk_r = vector3df(0, 0, -atan((xd - xa) / (yd - ya)));
			else
				mainGame->atk_r = vector3df(0, 0, 3.1415926 - atan((xd - xa) / (yd - ya)));
		}
		matManager.GenArrow(sy);
		mainGame->attack_sv = 0;
		mainGame->is_attacking = true;
		mainGame->WaitFrameSignal(40);
		mainGame->is_attacking = false;
		return true;
	}
	case MSG_BATTLE: {
		loc_info info1 = ClientCard::read_location_info(pbuf);
		info1.controler = mainGame->LocalPlayer(info1.controler);
		int aatk = BufferIO::ReadInt32(pbuf);
		int adef = BufferIO::ReadInt32(pbuf);
		/*int da = */BufferIO::ReadInt8(pbuf);
		loc_info info2 = ClientCard::read_location_info(pbuf);
		info2.controler = mainGame->LocalPlayer(info2.controler);
		int datk = BufferIO::ReadInt32(pbuf);
		int ddef = BufferIO::ReadInt32(pbuf);
		/*int dd = */BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		mainGame->gMutex.Lock();
		ClientCard* pcard = mainGame->dField.GetCard(info1.controler, info1.location, info1.sequence);
		if(aatk != pcard->attack) {
			pcard->attack = aatk;
			myswprintf(pcard->atkstring, L"%d", aatk);
		}
		if(adef != pcard->defense) {
			pcard->defense = adef;
			myswprintf(pcard->defstring, L"%d", adef);
		}
		if(info2.location) {
			pcard = mainGame->dField.GetCard(info2.controler, info2.location, info2.sequence);
			if(datk != pcard->attack) {
				pcard->attack = datk;
				myswprintf(pcard->atkstring, L"%d", datk);
			}
			if(ddef != pcard->defense) {
				pcard->defense = ddef;
				myswprintf(pcard->defstring, L"%d", ddef);
			}
		}
		mainGame->gMutex.Unlock();
		return true;
	}
	case MSG_ATTACK_DISABLED: {
		myswprintf(event_string, dataManager.GetSysString(1621), dataManager.GetName(mainGame->dField.attacker->code));
		return true;
	}
	case MSG_DAMAGE_STEP_START: {
		return true;
	}
	case MSG_DAMAGE_STEP_END: {
		return true;
	}
	case MSG_MISSED_EFFECT: {
		ClientCard::read_location_info(pbuf);
		unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
		myswprintf(textBuffer, dataManager.GetSysString(1622), dataManager.GetName(code));
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(code);
		return true;
	}
	case MSG_TOSS_COIN: {
		mainGame->PlaySoundEffect("./sound/coinflip.wav");
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		wchar_t* pwbuf = textBuffer;
		BufferIO::CopyWStrRef(dataManager.GetSysString(1623), pwbuf, 256);
		for (int i = 0; i < count; ++i) {
			int res = BufferIO::ReadInt8(pbuf);
			*pwbuf++ = L'[';
			BufferIO::CopyWStrRef(dataManager.GetSysString(res ? 60 : 61), pwbuf, 256);
			*pwbuf++ = L']';
		}
		*pwbuf = 0;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		mainGame->gMutex.Lock();
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(0);
		mainGame->stACMessage->setText(textBuffer);
		mainGame->PopupElement(mainGame->wACMessage, 20);
		mainGame->gMutex.Unlock();
		mainGame->WaitFrameSignal(40);
		return true;
	}
	case MSG_TOSS_DICE: {
		mainGame->PlaySoundEffect("./sound/diceroll.wav");
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		wchar_t* pwbuf = textBuffer;
		BufferIO::CopyWStrRef(dataManager.GetSysString(1624), pwbuf, 256);
		for (int i = 0; i < count; ++i) {
			int res = BufferIO::ReadInt8(pbuf);
			*pwbuf++ = L'[';
			*pwbuf++ = L'0' + res;
			*pwbuf++ = L']';
		}
		*pwbuf = 0;
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		mainGame->gMutex.Lock();
		mainGame->lstLog->addItem(textBuffer);
		mainGame->logParam.push_back(0);
		mainGame->stACMessage->setText(textBuffer);
		mainGame->PopupElement(mainGame->wACMessage, 20);
		mainGame->gMutex.Unlock();
		mainGame->WaitFrameSignal(40);
		return true;
	}
	case MSG_ROCK_PAPER_SCISSORS: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		mainGame->gMutex.Lock();
		mainGame->PopupElement(mainGame->wHand);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_HAND_RES: {
		int res = BufferIO::ReadInt8(pbuf);
		if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
			return true;
		mainGame->stHintMsg->setVisible(false);
		int res1 = (res & 0x3) - 1;
		int res2 = ((res >> 2) & 0x3) - 1;
		if(mainGame->dInfo.isFirst)
			mainGame->showcardcode = res1 + (res2 << 16);
		else
			mainGame->showcardcode = res2 + (res1 << 16);
		mainGame->showcarddif = 50;
		mainGame->showcardp = 0;
		mainGame->showcard = 100;
		mainGame->WaitFrameSignal(60);
		return false;
	}
	case MSG_ANNOUNCE_RACE: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		mainGame->dField.announce_count = BufferIO::ReadInt8(pbuf);
		int available = BufferIO::ReadInt32(pbuf);
		for(int i = 0, filter = 0x1; i < 25; ++i, filter <<= 1) {
			mainGame->chkRace[i]->setChecked(false);
			if(filter & available)
				mainGame->chkRace[i]->setVisible(true);
			else mainGame->chkRace[i]->setVisible(false);
		}
		if(select_hint)
			myswprintf(textBuffer, L"%ls", dataManager.GetDesc(select_hint));
		else myswprintf(textBuffer, dataManager.GetSysString(563));
		select_hint = 0;
		mainGame->gMutex.Lock();
		mainGame->wANRace->setText(textBuffer);
		mainGame->PopupElement(mainGame->wANRace);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_ANNOUNCE_ATTRIB: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		mainGame->dField.announce_count = BufferIO::ReadInt8(pbuf);
		int available = BufferIO::ReadInt32(pbuf);
		for(int i = 0, filter = 0x1; i < 7; ++i, filter <<= 1) {
			mainGame->chkAttribute[i]->setChecked(false);
			if(filter & available)
				mainGame->chkAttribute[i]->setVisible(true);
			else mainGame->chkAttribute[i]->setVisible(false);
		}
		if(select_hint)
			myswprintf(textBuffer, L"%ls", dataManager.GetDesc(select_hint));
		else myswprintf(textBuffer, dataManager.GetSysString(562));
		select_hint = 0;
		mainGame->gMutex.Lock();
		mainGame->wANAttribute->setText(textBuffer);
		mainGame->PopupElement(mainGame->wANAttribute);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_ANNOUNCE_CARD: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		mainGame->dField.declarable_type = BufferIO::ReadInt32(pbuf);
		mainGame->dField.opcode.clear();
		if(select_hint)
			myswprintf(textBuffer, L"%ls", dataManager.GetDesc(select_hint));
		else myswprintf(textBuffer, dataManager.GetSysString(564));
		select_hint = 0;
		mainGame->gMutex.Lock();
		mainGame->ebANCard->setText(L"");
		mainGame->wANCard->setText(textBuffer);
		mainGame->dField.UpdateDeclarableCode(false);
		mainGame->PopupElement(mainGame->wANCard);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_ANNOUNCE_NUMBER: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		mainGame->gMutex.Lock();
		mainGame->cbANNumber->clear();
		for (int i = 0; i < count; ++i) {
			u32 value = (mainGame->dInfo.lua64) ? (u32)BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
			myswprintf(textBuffer, L" %d", value);
			mainGame->cbANNumber->addItem(textBuffer, value);
		}
		mainGame->cbANNumber->setSelected(0);
		if(select_hint)
			myswprintf(textBuffer, L"%ls", dataManager.GetDesc(select_hint));
		else myswprintf(textBuffer, dataManager.GetSysString(565));
		select_hint = 0;
		mainGame->wANNumber->setText(textBuffer);
		mainGame->PopupElement(mainGame->wANNumber);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_ANNOUNCE_CARD_FILTER: {
		/*int player = */mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int count = BufferIO::ReadInt8(pbuf);
		mainGame->dField.declarable_type = 0;
		mainGame->dField.opcode.clear();
		for (int i = 0; i < count; ++i)
			mainGame->dField.opcode.push_back((mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf));
		if(select_hint)
			myswprintf(textBuffer, L"%ls", dataManager.GetDesc(select_hint));
		else myswprintf(textBuffer, dataManager.GetSysString(564));
		select_hint = 0;
		mainGame->gMutex.Lock();
		mainGame->ebANCard->setText(L"");
		mainGame->wANCard->setText(textBuffer);
		mainGame->dField.UpdateDeclarableCode(false);
		mainGame->PopupElement(mainGame->wANCard);
		mainGame->gMutex.Unlock();
		return false;
	}
	case MSG_CARD_HINT: {
		loc_info info = ClientCard::read_location_info(pbuf);
		int chtype = BufferIO::ReadInt8(pbuf);
		u64 value = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
		ClientCard* pcard = mainGame->dField.GetCard(mainGame->LocalPlayer(info.controler), info.location, info.sequence);
		if(!pcard)
			return true;
		if(chtype == CHINT_DESC_ADD) {
			pcard->desc_hints[value]++;
		} else if(chtype == CHINT_DESC_REMOVE) {
			pcard->desc_hints[value]--;
			if(pcard->desc_hints[value] == 0)
				pcard->desc_hints.erase(value);
		} else {
			pcard->cHint = chtype;
			pcard->chValue = value;
			if(chtype == CHINT_TURN) {
				if(value == 0)
					return true;
				if(mainGame->dInfo.isReplay && mainGame->dInfo.isReplaySkiping)
					return true;
				if(pcard->location & LOCATION_ONFIELD)
					pcard->is_highlighting = true;
				mainGame->showcardcode = pcard->code;
				mainGame->showcarddif = 0;
				mainGame->showcardp = value - 1;
				mainGame->showcard = 6;
				mainGame->WaitFrameSignal(30);
				pcard->is_highlighting = false;
				mainGame->showcard = 0;
			}
		}
		return true;
	}
	case MSG_PLAYER_HINT: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		int chtype = BufferIO::ReadInt8(pbuf);
		u64 value = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
		auto& player_desc_hints = mainGame->dField.player_desc_hints[player];
		if(chtype == PHINT_DESC_ADD) {
			player_desc_hints[value]++;
		} else if(chtype == PHINT_DESC_REMOVE) {
			player_desc_hints[value]--;
			if(player_desc_hints[value] == 0)
				player_desc_hints.erase(value);
		}
		return true;
	}
	case MSG_MATCH_KILL: {
		match_kill = BufferIO::ReadInt32(pbuf);
		return true;
	}
	case MSG_TAG_SWAP: {
		int player = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
		size_t mcount = (size_t)BufferIO::ReadInt8(pbuf);
		size_t ecount = (size_t)BufferIO::ReadInt8(pbuf);
		size_t pcount = (size_t)BufferIO::ReadInt8(pbuf);
		size_t hcount = (size_t)BufferIO::ReadInt8(pbuf);
		int topcode = BufferIO::ReadInt32(pbuf);
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ++cit) {
				if(player == 0) (*cit)->dPos.Y = 0.4f;
				else (*cit)->dPos.Y = -0.6f;
				(*cit)->dRot = irr::core::vector3df(0, 0, 0);
				(*cit)->is_moving = true;
				(*cit)->aniFrame = 5;
			}
			for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit) {
				if(player == 0) (*cit)->dPos.Y = 0.4f;
				else (*cit)->dPos.Y = -0.6f;
				(*cit)->dRot = irr::core::vector3df(0, 0, 0);
				(*cit)->is_moving = true;
				(*cit)->aniFrame = 5;
			}
			for (auto cit = mainGame->dField.extra[player].begin(); cit != mainGame->dField.extra[player].end(); ++cit) {
				if(player == 0) (*cit)->dPos.Y = 0.4f;
				else (*cit)->dPos.Y = -0.6f;
				(*cit)->dRot = irr::core::vector3df(0, 0, 0);
				(*cit)->is_moving = true;
				(*cit)->aniFrame = 5;
			}
			mainGame->WaitFrameSignal(5);
		}
		//
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Lock();
		if(mainGame->dField.deck[player].size() > mcount) {
			while(mainGame->dField.deck[player].size() > mcount) {
				ClientCard* ccard = *mainGame->dField.deck[player].rbegin();
				mainGame->dField.deck[player].pop_back();
				delete ccard;
			}
		} else {
			while(mainGame->dField.deck[player].size() < mcount) {
				ClientCard* ccard = new ClientCard();
				ccard->controler = player;
				ccard->location = LOCATION_DECK;
				ccard->sequence = mainGame->dField.deck[player].size();
				mainGame->dField.deck[player].push_back(ccard);
			}
		}
		if(mainGame->dField.hand[player].size() > hcount) {
			while(mainGame->dField.hand[player].size() > hcount) {
				ClientCard* ccard = *mainGame->dField.hand[player].rbegin();
				mainGame->dField.hand[player].pop_back();
				delete ccard;
			}
		} else {
			while(mainGame->dField.hand[player].size() < hcount) {
				ClientCard* ccard = new ClientCard();
				ccard->controler = player;
				ccard->location = LOCATION_HAND;
				ccard->sequence = mainGame->dField.hand[player].size();
				mainGame->dField.hand[player].push_back(ccard);
			}
		}
		if(mainGame->dField.extra[player].size() > ecount) {
			while(mainGame->dField.extra[player].size() > ecount) {
				ClientCard* ccard = *mainGame->dField.extra[player].rbegin();
				mainGame->dField.extra[player].pop_back();
				delete ccard;
			}
		} else {
			while(mainGame->dField.extra[player].size() < ecount) {
				ClientCard* ccard = new ClientCard();
				ccard->controler = player;
				ccard->location = LOCATION_EXTRA;
				ccard->sequence = mainGame->dField.extra[player].size();
				mainGame->dField.extra[player].push_back(ccard);
			}
		}
		mainGame->dField.extra_p_count[player] = pcount;
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Unlock();
		//
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping) {
			for (auto cit = mainGame->dField.deck[player].begin(); cit != mainGame->dField.deck[player].end(); ++cit) {
				ClientCard* pcard = *cit;
				mainGame->dField.GetCardLocation(pcard, &pcard->curPos, &pcard->curRot);
				if(player == 0) pcard->curPos.Y += 2.0f;
				else pcard->curPos.Y -= 3.0f;
				mainGame->dField.MoveCard(*cit, 5);
			}
			if(mainGame->dField.deck[player].size())
				(*mainGame->dField.deck[player].rbegin())->code = topcode;
			for (auto cit = mainGame->dField.hand[player].begin(); cit != mainGame->dField.hand[player].end(); ++cit) {
				ClientCard* pcard = *cit;
				pcard->code = BufferIO::ReadInt32(pbuf);
				mainGame->dField.GetCardLocation(pcard, &pcard->curPos, &pcard->curRot);
				if(player == 0) pcard->curPos.Y += 2.0f;
				else pcard->curPos.Y -= 3.0f;
				mainGame->dField.MoveCard(*cit, 5);
			}
			for (auto cit = mainGame->dField.extra[player].begin(); cit != mainGame->dField.extra[player].end(); ++cit) {
				ClientCard* pcard = *cit;
				pcard->code = BufferIO::ReadInt32(pbuf) & 0x7fffffff;
				mainGame->dField.GetCardLocation(pcard, &pcard->curPos, &pcard->curRot);
				if(player == 0) pcard->curPos.Y += 2.0f;
				else pcard->curPos.Y -= 3.0f;
				mainGame->dField.MoveCard(*cit, 5);
			}
			mainGame->WaitFrameSignal(5);
		}
		if(mainGame->dInfo.isRelay)
			mainGame->dInfo.current_player[player]++;
		break;
	}
	case MSG_RELOAD_FIELD: {
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Lock();
		mainGame->dField.Clear();
		int field = BufferIO::ReadInt8(pbuf);
		mainGame->dInfo.duel_field = field & 0xf;
		mainGame->dInfo.extraval = field >> 4;
		mainGame->SetPhaseButtons();
		int val = 0;
		for(int i = 0; i < 2; ++i) {
			int p = mainGame->LocalPlayer(i);
			mainGame->dInfo.lp[p] = BufferIO::ReadInt32(pbuf);
			myswprintf(mainGame->dInfo.strLP[p], L"%d", mainGame->dInfo.lp[p]);
			for(int seq = 0; seq < 7; ++seq) {
				val = BufferIO::ReadInt8(pbuf);
				if(val) {
					ClientCard* ccard = new ClientCard;
					mainGame->dField.AddCard(ccard, p, LOCATION_MZONE, seq);
					ccard->position = BufferIO::ReadInt8(pbuf);
					val = BufferIO::ReadInt8(pbuf);
					if(val) {
						for(int xyz = 0; xyz < val; ++xyz) {
							ClientCard* xcard = new ClientCard;
							ccard->overlayed.push_back(xcard);
							mainGame->dField.overlay_cards.insert(xcard);
							xcard->overlayTarget = ccard;
							xcard->location = LOCATION_OVERLAY;
							xcard->sequence = ccard->overlayed.size() - 1;
							xcard->owner = p;
							xcard->controler = p;
						}
					}
				}
			}
			for(int seq = 0; seq < 8; ++seq) {
				val = BufferIO::ReadInt8(pbuf);
				if(val) {
					ClientCard* ccard = new ClientCard;
					mainGame->dField.AddCard(ccard, p, LOCATION_SZONE, seq);
					ccard->position = BufferIO::ReadInt8(pbuf);
				}
			}
			val = BufferIO::ReadInt16(pbuf);
			for(int seq = 0; seq < val; ++seq) {
				ClientCard* ccard = new ClientCard;
				mainGame->dField.AddCard(ccard, p, LOCATION_DECK, seq);
			}
			val = BufferIO::ReadInt16(pbuf);
			for(int seq = 0; seq < val; ++seq) {
				ClientCard* ccard = new ClientCard;
				mainGame->dField.AddCard(ccard, p, LOCATION_HAND, seq);
			}
			val = BufferIO::ReadInt16(pbuf);
			for(int seq = 0; seq < val; ++seq) {
				ClientCard* ccard = new ClientCard;
				mainGame->dField.AddCard(ccard, p, LOCATION_GRAVE, seq);
			}
			val = BufferIO::ReadInt16(pbuf);
			for(int seq = 0; seq < val; ++seq) {
				ClientCard* ccard = new ClientCard;
				mainGame->dField.AddCard(ccard, p, LOCATION_REMOVED, seq);
			}
			val = BufferIO::ReadInt16(pbuf);
			for(int seq = 0; seq < val; ++seq) {
				ClientCard* ccard = new ClientCard;
				mainGame->dField.AddCard(ccard, p, LOCATION_EXTRA, seq);
			}
			val = BufferIO::ReadInt8(pbuf);
			mainGame->dField.extra_p_count[p] = val;
		}
		mainGame->dField.RefreshAllCards();
		val = BufferIO::ReadInt8(pbuf); //chains, always 0 in single mode
		if(!mainGame->dInfo.isSingleMode) {
			for(int i = 0; i < val; ++i) {
				unsigned int code = (unsigned int)BufferIO::ReadInt32(pbuf);
				int pcc = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
				int pcl = BufferIO::ReadInt8(pbuf);
				int pcs = BufferIO::ReadInt8(pbuf);
				int subs = BufferIO::ReadInt8(pbuf);
				int cc = mainGame->LocalPlayer(BufferIO::ReadInt8(pbuf));
				int cl = BufferIO::ReadInt8(pbuf);
				int cs = BufferIO::ReadInt8(pbuf);
				u64 desc = (mainGame->dInfo.lua64) ? BufferIO::ReadInt64(pbuf) : BufferIO::ReadInt32(pbuf);
				ClientCard* pcard = mainGame->dField.GetCard(pcc, pcl, pcs, subs);
				mainGame->dField.current_chain.chain_card = pcard;
				mainGame->dField.current_chain.code = code;
				mainGame->dField.current_chain.desc = desc;
				mainGame->dField.current_chain.controler = cc;
				mainGame->dField.current_chain.location = cl;
				mainGame->dField.current_chain.sequence = cs;
				mainGame->dField.GetChainLocation(cc, cl, cs, &mainGame->dField.current_chain.chain_pos);
				mainGame->dField.current_chain.solved = false;
				int chc = 0;
				for(auto chit = mainGame->dField.chains.begin(); chit != mainGame->dField.chains.end(); ++chit) {
					if (cl == LOCATION_GRAVE || cl == LOCATION_REMOVED) {
						if (chit->controler == cc && chit->location == cl)
							chc++;
					} else {
						if (chit->controler == cc && chit->location == cl && chit->sequence == cs)
							chc++;
					}
				}
				if(cl == LOCATION_HAND)
					mainGame->dField.current_chain.chain_pos.X += 0.35f;
				else
					mainGame->dField.current_chain.chain_pos.Y += chc * 0.25f;
				mainGame->dField.chains.push_back(mainGame->dField.current_chain);
			}
			if(val) {
				myswprintf(event_string, dataManager.GetSysString(1609), dataManager.GetName(mainGame->dField.current_chain.code));
				mainGame->dField.last_chain = true;
			}
		}
		if(!mainGame->dInfo.isReplay || !mainGame->dInfo.isReplaySkiping)
			mainGame->gMutex.Unlock();
		break;
	}
	}
	return true;
}
void DuelClient::SetResponseI(int respI) {
	*((int*)response_buf) = respI;
	response_len = 4;
}
void DuelClient::SetResponseB(void* respB, unsigned char len) {
	memcpy(response_buf, respB, len);
	response_len = len;
}
void DuelClient::SendResponse() {
	switch(mainGame->dInfo.curMsg) {
	case MSG_SELECT_BATTLECMD: {
		mainGame->dField.ClearCommandFlag();
		mainGame->btnM2->setVisible(false);
		mainGame->btnEP->setVisible(false);
		break;
	}
	case MSG_SELECT_IDLECMD: {
		mainGame->dField.ClearCommandFlag();
		mainGame->btnBP->setVisible(false);
		mainGame->btnEP->setVisible(false);
		mainGame->btnShuffle->setVisible(false);
		break;
	}
	case MSG_SELECT_CARD:
	case MSG_SELECT_UNSELECT_CARD: {
		mainGame->dField.ClearSelect();
		for (auto cit = mainGame->dField.limbo_temp.begin(); cit != mainGame->dField.limbo_temp.end(); ++cit)
			delete *cit;
		mainGame->dField.limbo_temp.clear();
		break;
	}
	case MSG_SELECT_CHAIN: {
		mainGame->dField.ClearChainSelect();
		break;
	}
	case MSG_SELECT_TRIBUTE: {
		mainGame->dField.ClearSelect();
		break;
	}
	case MSG_SELECT_COUNTER: {
		mainGame->dField.ClearSelect();
		break;
	}
	case MSG_SELECT_SUM: {
		for(int i = 0; i < mainGame->dField.must_select_cards.size(); ++i) {
			mainGame->dField.must_select_cards[i]->is_selected = false;
		}
		for(size_t i = 0; i < mainGame->dField.selectsum_all.size(); ++i) {
			mainGame->dField.selectsum_all[i]->is_selectable = false;
			mainGame->dField.selectsum_all[i]->is_selected = false;
		}
		break;
	}
	case MSG_CONFIRM_CARDS: {
		for (auto cit = mainGame->dField.limbo_temp.begin(); cit != mainGame->dField.limbo_temp.end(); ++cit)
			delete *cit;
		mainGame->dField.limbo_temp.clear();
		break;
	}
	}
	replay_stream.pop_back();
	if(mainGame->dInfo.isSingleMode) {
		SingleMode::SetResponse(response_buf, response_len);
		mainGame->singleSignal.Set();
	} else if (!mainGame->dInfo.isReplay) {
		mainGame->dInfo.time_player = 2;
		SendBufferToServer(CTOS_RESPONSE, response_buf, response_len);
	}
}
void DuelClient::BeginRefreshHost() {
	if(is_refreshing)
		return;
	is_refreshing = true;
	mainGame->btnLanRefresh->setEnabled(false);
	mainGame->lstHostList->clear();
	remotes.clear();
	hosts.clear();
	event_base* broadev = event_base_new();
	char hname[256];
	gethostname(hname, 256);
	hostent* host = gethostbyname(hname);
	if(!host)
		return;
	SOCKET reply = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in reply_addr;
	memset(&reply_addr, 0, sizeof(reply_addr));
	reply_addr.sin_family = AF_INET;
	reply_addr.sin_port = htons(7921);
	reply_addr.sin_addr.s_addr = 0;
	if(bind(reply, (sockaddr*)&reply_addr, sizeof(reply_addr)) == SOCKET_ERROR) {
		closesocket(reply);
		return;
	}
	timeval timeout = {3, 0};
	resp_event = event_new(broadev, reply, EV_TIMEOUT | EV_READ | EV_PERSIST, BroadcastReply, broadev);
	event_add(resp_event, &timeout);
	Thread::NewThread(RefreshThread, broadev);
	//send request
	SOCKADDR_IN local;
	local.sin_family = AF_INET;
	local.sin_port = htons(7922);
	SOCKADDR_IN sockTo;
	sockTo.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sockTo.sin_family = AF_INET;
	sockTo.sin_port = htons(7920);
	HostRequest hReq;
	hReq.identifier = NETWORK_CLIENT_ID;
	for(int i = 0; i < 8; ++i) {
		if(host->h_addr_list[i] == 0)
			break;
		unsigned int local_addr = *(unsigned int*)host->h_addr_list[i];
		local.sin_addr.s_addr = local_addr;
		SOCKET sSend = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if(sSend == INVALID_SOCKET)
			break;
		BOOL opt = TRUE;
		setsockopt(sSend, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(BOOL));
		if(bind(sSend, (sockaddr*)&local, sizeof(sockaddr)) == SOCKET_ERROR) {
			closesocket(sSend);
			break;
		}
		sendto(sSend, (const char*)&hReq, sizeof(HostRequest), 0, (sockaddr*)&sockTo, sizeof(sockaddr));
		closesocket(sSend);
	}
}
int DuelClient::RefreshThread(void * arg) {
	event_base* broadev = (event_base*)arg;
	event_base_dispatch(broadev);
	evutil_socket_t fd;
	event_get_assignment(resp_event, 0, &fd, 0, 0, 0);
	evutil_closesocket(fd);
	event_free(resp_event);
	event_base_free(broadev);
	is_refreshing = false;
	return 0;
}
void DuelClient::BroadcastReply(evutil_socket_t fd, short events, void * arg) {
	if(events & EV_TIMEOUT) {
		evutil_closesocket(fd);
		event_base_loopbreak((event_base*)arg);
		if(!is_closing)
			mainGame->btnLanRefresh->setEnabled(true);
	} else if(events & EV_READ) {
		sockaddr_in bc_addr;
		socklen_t sz = sizeof(sockaddr_in);
		char buf[256];
		/*int ret = */recvfrom(fd, buf, 256, 0, (sockaddr*)&bc_addr, &sz);
		unsigned int ipaddr = bc_addr.sin_addr.s_addr;
		HostPacket* pHP = (HostPacket*)buf;
		if(!is_closing && pHP->identifier == NETWORK_SERVER_ID && remotes.find(ipaddr) == remotes.end() ) {
			wchar_t msgbuf[256];
			mainGame->gMutex.Lock();
			remotes.insert(ipaddr);
			pHP->ipaddr = ipaddr;
			hosts.push_back(*pHP);
			std::wstring hoststr;
			hoststr.append(L"[");
			hoststr.append(deckManager.GetLFListName(pHP->host.lflist));
			hoststr.append(L"][");
			hoststr.append(dataManager.GetSysString(pHP->host.rule + 1240));
			hoststr.append(L"][");
			hoststr.append(dataManager.GetSysString(pHP->host.mode + 1244));
			hoststr.append(L"][");
			myswprintf(msgbuf, L"%X.0%X.%X", pHP->version >> 12, (pHP->version >> 4) & 0xff, pHP->version & 0xf);
			hoststr.append(msgbuf);
			hoststr.append(L"][");
			int rule;
			if(pHP->host.check == 2) {
				mainGame->GetMasterRule(pHP->host.duel_flag, pHP->host.forbiddentypes, &rule);
			} else
				rule = pHP->host.duel_rule;
			if(rule == 5)
				myswprintf(msgbuf, L"Custom MR");
			else
				myswprintf(msgbuf, L"MR %d", (rule == 0) ? 3 : rule);
			hoststr.append(msgbuf);
			hoststr.append(L"][");
			if(pHP->host.draw_count == 1 && pHP->host.start_hand == 5 && pHP->host.start_lp == 8000
			        && !pHP->host.no_check_deck && !pHP->host.no_shuffle_deck 
					&& rule == DEFAULT_DUEL_RULE && pHP->host.extra_rules==0)
				hoststr.append(dataManager.GetSysString(1280));
			else hoststr.append(dataManager.GetSysString(1281));
			hoststr.append(L"]");
			wchar_t gamename[20];
			BufferIO::CopyWStr(pHP->name, gamename, 20);
			hoststr.append(gamename);
			mainGame->lstHostList->addItem(hoststr.c_str());
			mainGame->gMutex.Unlock();
		}
	}
}
void DuelClient::ReplayPrompt(bool need_header) {
	if(need_header) {
		ReplayHeader pheader;
		pheader.id = 0x58707279;
		pheader.version = PRO_VERSION;
		pheader.flag = REPLAY_LUA64 | REPLAY_NEWREPLAY;
		if(mainGame->dInfo.isTag)
			pheader.flag |= REPLAY_TAG;
		if(mainGame->dInfo.isRelay)
			pheader.flag |= REPLAY_RELAY;
		last_replay.BeginRecord(false);
		last_replay.WriteHeader(pheader);
		last_replay.pheader.id = 0x58707279;
		if(last_replay.pheader.flag & REPLAY_TAG) {
			last_replay.WriteData(mainGame->dInfo.hostname[0], 40, false);
			last_replay.WriteData(mainGame->dInfo.hostname[1], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[0], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[1], 40, false);
		} else if(last_replay.pheader.flag & REPLAY_RELAY) {
			last_replay.WriteData(mainGame->dInfo.hostname[0], 40, false);
			last_replay.WriteData(mainGame->dInfo.hostname[1], 40, false);
			last_replay.WriteData(mainGame->dInfo.hostname[2], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[0], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[1], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[2], 40, false);
		} else {
			last_replay.WriteData(mainGame->dInfo.hostname[0], 40, false);
			last_replay.WriteData(mainGame->dInfo.clientname[0], 40, false);
		}
		last_replay.WriteInt32(mainGame->dInfo.duel_field | mainGame->dInfo.extraval >> 8);
		last_replay.WriteStream(replay_stream);
		last_replay.EndRecord();
	}
	mainGame->gMutex.Lock();
	mainGame->wPhase->setVisible(false);
	if(mainGame->dInfo.player_type < 7)
		mainGame->btnLeaveGame->setVisible(false);
	mainGame->btnChainIgnore->setVisible(false);
	mainGame->btnChainAlways->setVisible(false);
	mainGame->btnChainWhenAvail->setVisible(false);
	mainGame->btnCancelOrFinish->setVisible(false);
	time_t nowtime = time(NULL);
	struct tm *localedtime = localtime(&nowtime);
	char timebuf[40];
	strftime(timebuf, 40, "%Y-%m-%d %H-%M-%S", localedtime);
	size_t size = strlen(timebuf) + 1;
	wchar_t timetext[80];
	mbstowcs(timetext, timebuf, size);
	mainGame->ebRSName->setText(timetext);
	mainGame->wReplaySave->setText(dataManager.GetSysString(1340));
	mainGame->PopupElement(mainGame->wReplaySave);
	mainGame->gMutex.Unlock();
	mainGame->replaySignal.Reset();
	mainGame->replaySignal.Wait();
	if(mainGame->saveReplay || !is_host) {
		if(mainGame->saveReplay)
			last_replay.SaveReplay(mainGame->ebRSName->getText());
		else last_replay.SaveReplay(L"_LastReplay");
	}

}
}
